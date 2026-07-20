#include "rtsp_av_client.hpp"

#include <chrono>
#include <cstdint>
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
// BGR 변환·큐 전달 상한 fps. 서버 파이프라인은 채널당 ~15fps만 소비하므로
// (main.cpp의 kMainProcessInterval) 그 이상은 변환해 봐야 버려진다.
// 디코딩 자체는 H.264 참조 프레임 때문에 전 프레임 필수지만, sws_scale
// 변환과 Mat 할당은 여기서 걸러 스킵한다 (고fps 입력 카메라 대비 안전판 —
// 카메라 프로파일이 15fps면 사실상 전부 통과).
// main 쪽 15fps 스로틀이 최종 관문이므로 여기는 살짝 여유(18fps)를 둔다.
constexpr double kMaxConvertFps = 18.0;
// (실험 기록) sws_scale에서 바로 960x540으로 다운스케일해 봤으나, MoveNet
// 크롭 해상도가 같이 떨어져 원거리 사람의 자세 감지가 불안정해짐 → 원복.
// 원본 해상도로 변환하고 GUI용 축소는 main.cpp의 cv::resize가 담당한다.
// 메타데이터 재조립 버퍼 상한 — 정상 문서는 수 KB, 이걸 넘으면 스트림 이상
constexpr size_t kMetaBufMax = 256 * 1024;

// 연결 옵션: RTSP over TCP만 지정.
// 주의: 이 ffmpeg 버전에선 stimeout/timeout 옵션이 RTSP를 리스닝(서버) 모드로
// 전환시켜 "Unable to open RTSP for listening" 오류를 낸다. 성공하는 CLI
// (ffprobe -rtsp_transport tcp)와 동일하게 transport만 준다.
void setRtspOptions(AVDictionary** opts) {
    av_dict_set(opts, "rtsp_transport", "tcp", 0);
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
    // RTSP/TCP 등 네트워크 프로토콜 사용 준비 (프로세스당 1회면 충분, 중복 호출 무해)
    avformat_network_init();
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

    // 1차: rtsp_transport=tcp 로 시도. 실패 시 옵션 없이 재시도(진단 겸 폴백).
    AVDictionary* opts = nullptr;
    setRtspOptions(&opts);
    int rc = avformat_open_input(&fmt, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        char err[128] = {0};
        av_strerror(rc, err, sizeof(err));
        std::fprintf(stderr, "[ch%d] tcp 옵션 연결 실패 (%d: %s) — 옵션 없이 재시도\n",
                     channel_, rc, err);

        if (fmt) {
            avformat_close_input(&fmt);
            fmt = nullptr;
        }
        rc = avformat_open_input(&fmt, url_.c_str(), nullptr, nullptr);
        if (rc < 0) {
            char err2[128] = {0};
            av_strerror(rc, err2, sizeof(err2));
            std::fprintf(stderr, "[ch%d] RTSP 연결 실패 (%d: %s)\n",
                         channel_, rc, err2);
            if (fmt) avformat_close_input(&fmt);
            return false;
        }
        std::fprintf(stderr, "[ch%d] 옵션 없이 연결 성공 (기본 transport)\n", channel_);
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
    if (on_stream_ready_) {
        AVRational tb = fmt->streams[video_idx]->time_base;
        on_stream_ready_(vpar, tb.num, tb.den);
    }
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

    // ── PTS(촬영 시각) 동기화 ──────────────────────────────────────
    // 영상은 디코딩·GOV 버퍼링 지연으로 늦게 도착하지만 메타는 즉시 온다.
    // 수신 시각(now())으로 매칭하면 블러가 사람보다 밀린다. 그래서 두 트랙 모두
    // 카메라가 붙인 PTS를 공통 앵커로 steady_clock에 매핑해, 도착 지연과 무관하게
    // 같은 촬영 시각으로 정렬한다. PTS가 없으면 now()로 폴백.
    const double vtb_sec = av_q2d(fmt->streams[video_idx]->time_base);
    const double dtb_sec =
        (data_idx >= 0) ? av_q2d(fmt->streams[data_idx]->time_base) : 0.0;
    bool pts_anchored = false;
    double pts0_sec = 0.0;
    std::chrono::steady_clock::time_point pts_epoch;
    auto ptsToCapture = [&](double pts_sec) {
        if (!pts_anchored) {
            pts0_sec = pts_sec;
            pts_epoch = std::chrono::steady_clock::now();
            pts_anchored = true;
        }
        return pts_epoch +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::duration<double>(pts_sec - pts0_sec));
    };

    auto last_convert = std::chrono::steady_clock::time_point{};
    const auto convert_interval =
        std::chrono::duration<double>(1.0 / kMaxConvertFps);

    connected_.store(true);
    meta_buf_.clear();  // 재연결 시 이전 연결의 조각 폐기
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
            // 압축 상태 그대로 블랙박스 소비자에게 전달 (디코딩 스로틀과 무관하게 전 패킷)
            if (on_packet_) on_packet_(pkt);

            // ── 영상 패킷: 디코딩 → cv::Mat → 큐 ──
            if (avcodec_send_packet(dctx, pkt) == 0) {
                while (avcodec_receive_frame(dctx, frame) == 0) {
                    frame_count_.fetch_add(1);

                    // 소비 안 될 프레임은 BGR 변환 없이 버린다 (위 kMaxConvertFps 주석 참조)
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_convert < convert_interval) continue;
                    last_convert = now;

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

                    // 프레임 PTS → 촬영 시각 (없으면 수신 시각 폴백)
                    int64_t vts = frame->best_effort_timestamp;
                    if (vts == AV_NOPTS_VALUE) vts = frame->pts;
                    auto captured = (vts != AV_NOPTS_VALUE)
                                        ? ptsToCapture(vts * vtb_sec)
                                        : std::chrono::steady_clock::now();
                    queue_.push(Frame{channel_, std::move(img), captured});
                }
            }
        } else if (pkt->stream_index == data_idx && data_idx >= 0) {
            // ── 메타데이터 패킷: 조각 재조립 → 완성 문서 단위 파싱 → 콜백 ──
            // 화면에 객체가 많으면 XML 한 문서가 여러 RTP 패킷으로 쪼개져 온다.
            // 패킷 하나를 바로 파싱하면 잘린 문서라 결과가 비어(사람0) 버리므로,
            // 문서 종료 태그가 나올 때까지 모았다가 완성본만 파싱한다.
            metadata_count_.fetch_add(1);
            if (on_detections_) {
                // 메타 패킷 PTS → 촬영 시각 (영상 프레임과 같은 타임라인).
                auto data_captured = (pkt->pts != AV_NOPTS_VALUE)
                                         ? ptsToCapture(pkt->pts * dtb_sec)
                                         : std::chrono::steady_clock::now();
                meta_buf_.append(reinterpret_cast<const char*>(pkt->data),
                                 static_cast<size_t>(pkt->size));
                static const std::string kDocEnd = "</tt:MetadataStream>";
                size_t end;
                while ((end = meta_buf_.find(kDocEnd)) != std::string::npos) {
                    size_t doc_len = end + kDocEnd.size();
                    auto dets = MetadataParser::parse(meta_buf_.substr(0, doc_len));
                    meta_buf_.erase(0, doc_len);
                    if (!dets.empty()) {
                        on_detections_(channel_, std::move(dets), data_captured);
                    }
                }
                if (meta_buf_.size() > kMetaBufMax) {
                    meta_buf_.clear();  // 종료 태그가 영영 안 오는 비정상 스트림 방어
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
