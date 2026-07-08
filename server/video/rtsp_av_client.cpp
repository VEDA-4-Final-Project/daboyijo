#include "rtsp_av_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <opencv2/core.hpp>

#include "metadata_parser.hpp"

namespace {
constexpr int kReconnectDelaySec = 3;

// 연결 옵션: RTSP over TCP + 타임아웃(마이크로초). 네트워크 끊김 시 무한 대기 방지.
// stimeout은 ffmpeg 5.0+에서 timeout으로 개명됨 — 둘 다 설정(미인식 옵션은 무시됨).
void setRtspOptions(AVDictionary** opts) {
    av_dict_set(opts, "rtsp_transport", "tcp", 0);
    av_dict_set(opts, "stimeout", "5000000", 0);   // 5초 소켓 타임아웃 (구버전)
    av_dict_set(opts, "timeout", "5000000", 0);    // 5초 소켓 타임아웃 (신버전)
    av_dict_set(opts, "max_delay", "500000", 0);   // 0.5초
}
}  // namespace

RtspAvClient::RtspAvClient(int channel, std::string url, FrameQueue& queue)
    : channel_(channel), url_(std::move(url)), queue_(queue) {}

RtspAvClient::~RtspAvClient() {
    stop();
}

void RtspAvClient::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&RtspAvClient::run, this);
}

void RtspAvClient::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RtspAvClient::run() {
    while (running_.load()) {
        if (!openAndStream() && running_.load()) {
            std::fprintf(stderr, "[ch%d] 재연결 %d초 대기\n", channel_,
                         kReconnectDelaySec);
            for (int i = 0; i < kReconnectDelaySec * 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

// 1회 연결 → 패킷 수신 루프. 정상 종료(stop)든 오류든 자원 정리 후 반환.
bool RtspAvClient::openAndStream() {
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    setRtspOptions(&opts);

    int rc = avformat_open_input(&fmt, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        std::fprintf(stderr, "[ch%d] RTSP 연결 실패\n", channel_);
        return false;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::fprintf(stderr, "[ch%d] 스트림 정보 조회 실패\n", channel_);
        avformat_close_input(&fmt);
        return false;
    }

    // 트랙 식별: 영상 트랙, 메타데이터(data) 트랙
    int video_idx = -1;
    int data_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVMediaType t = fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && video_idx < 0) {
            video_idx = static_cast<int>(i);
        } else if (t == AVMEDIA_TYPE_DATA && data_idx < 0) {
            data_idx = static_cast<int>(i);
        }
    }
    if (video_idx < 0) {
        std::fprintf(stderr, "[ch%d] 영상 트랙 없음\n", channel_);
        avformat_close_input(&fmt);
        return false;
    }

    // 영상 디코더 준비
    AVCodecParameters* vpar = fmt->streams[video_idx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(vpar->codec_id);
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dctx, vpar);
    // RPi 멀티코어 활용: 디코딩 스레드
    dctx->thread_count = 2;
    if (avcodec_open2(dctx, dec, nullptr) < 0) {
        std::fprintf(stderr, "[ch%d] 디코더 열기 실패\n", channel_);
        avcodec_free_context(&dctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;  // YUV→BGR 변환기 (첫 프레임에서 지연 생성)
    int sws_w = 0, sws_h = 0;

    connected_.store(true);
    std::fprintf(stderr, "[ch%d] 연결됨 (영상 트랙=%d, 메타 트랙=%d)\n",
                 channel_, video_idx, data_idx);

    bool ok = true;
    while (running_.load()) {
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            ok = false;  // 스트림 끊김 → 재연결
            break;
        }

        if (pkt->stream_index == video_idx) {
            // ── 영상 패킷: 디코딩 → cv::Mat → 큐 ──
            if (avcodec_send_packet(dctx, pkt) == 0) {
                while (avcodec_receive_frame(dctx, frame) == 0) {
                    if (!sws || sws_w != frame->width || sws_h != frame->height) {
                        if (sws) sws_freeContext(sws);
                        sws = sws_getContext(frame->width, frame->height,
                                             static_cast<AVPixelFormat>(frame->format),
                                             frame->width, frame->height,
                                             AV_PIX_FMT_BGR24, SWS_BILINEAR,
                                             nullptr, nullptr, nullptr);
                        sws_w = frame->width;
                        sws_h = frame->height;
                    }
                    cv::Mat img(frame->height, frame->width, CV_8UC3);
                    uint8_t* dst[1] = {img.data};
                    int dst_stride[1] = {static_cast<int>(img.step)};
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                              dst, dst_stride);

                    frame_count_.fetch_add(1);
                    queue_.push(Frame{channel_, std::move(img),
                                      std::chrono::steady_clock::now()});
                }
            }
        } else if (pkt->stream_index == data_idx && data_idx >= 0) {
            // ── 메타데이터 패킷: XML 파싱 → 콜백 ──
            metadata_count_.fetch_add(1);
            if (on_detections_) {
                std::string xml(reinterpret_cast<const char*>(pkt->data),
                                static_cast<size_t>(pkt->size));
                auto dets = MetadataParser::parse(xml);
                if (!dets.empty()) {
                    on_detections_(channel_, std::move(dets));
                }
            }
        }

        av_packet_unref(pkt);
    }

    connected_.store(false);
    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dctx);
    avformat_close_input(&fmt);
    return ok;
}
