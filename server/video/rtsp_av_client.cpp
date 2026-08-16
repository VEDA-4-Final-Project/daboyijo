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
// BGR 변환·큐 전달 상한 fps. 디코딩 자체는 H.264 참조 프레임 때문에 전 프레임
// 필수지만, sws_scale 변환과 Mat 할당은 여기서 걸러 스킵한다 (고fps 입력 대비 안전판).
// ★ 중요: 이 값은 반드시 입력 fps보다 "넉넉히" 위여야 한다. 입력 간격에 가까우면
//   (예: 입력 20fps=50ms, 캡 18fps=55.6ms) 지터 없이도 매 두 번째 프레임이
//   55.6>50 때문에 스킵돼 실효 fps가 절반(10fps)으로 주저앉는다.
//   입력 20fps 운용 기준 여유롭게 둔다. 특히 H.264는 B-프레임 재정렬+네트워크
//   배칭으로 디코딩 출력이 버스티(뭉쳐 나옴)해서, 벽시계 게이트 간격이 크면
//   버스트 내 프레임이 드랍돼 실효 fps가 주저앉는다. 카메라가 20fps로 상한이라
//   사실상 방어가 불필요 → 60fps(16.7ms)로 크게 열어 전 프레임을 통과시킨다.
//   (main.cpp의 kMainProcessInterval은 최종 폭주 방어로 30fps 유지)
constexpr double kMaxConvertFps = 60.0;
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
    url_cv_.notify_all();  // 대기 중이면 즉시 깨워 종료
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RtspAvClient::reconnect(std::string url) {
    {
        std::lock_guard<std::mutex> lk(url_mutex_);
        url_ = std::move(url);
    }
    reload_.store(true);   // 진행 중 스트림을 끊고 새 URL을 읽게 함
    url_cv_.notify_all();  // 대기 상태였다면 깨움
}

void RtspAvClient::disconnect() {
    {
        std::lock_guard<std::mutex> lk(url_mutex_);
        url_.clear();
    }
    reload_.store(true);   // 진행 중 스트림 중단 → run()이 대기 상태로 복귀
    url_cv_.notify_all();
}

int RtspAvClient::interruptCb(void* opaque) {
    auto* self = static_cast<RtspAvClient*>(opaque);
    // 정지 요청이거나 URL 교체 요청이면 블로킹 I/O(av_read_frame/open)를 즉시 중단.
    return (!self->running_.load() || self->reload_.load()) ? 1 : 0;
}

void RtspAvClient::run() {
    while (running_.load()) {
        std::string url;
        {
            std::unique_lock<std::mutex> lk(url_mutex_);
            if (url_.empty()) {
                // 카메라 미지정 — Qt의 "카메라 연결" 신호가 올 때까지 대기.
                url_cv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                    return !running_.load() || !url_.empty();
                });
                continue;
            }
            url = url_;
        }

        reload_.store(false);
        bool ok = openAndStream(url);

        // URL 교체(reload_)로 끊긴 경우엔 백오프 없이 즉시 새 URL로 재시도.
        // 그 외(네트워크 끊김 등)에는 기존처럼 재연결 대기.
        if (!ok && running_.load() && !reload_.load()) {
            std::fprintf(stderr, "[ch%d] 재연결 %d초 대기\n", channel_ + 1,
                         kReconnectDelaySec);
            for (int i = 0; i < kReconnectDelaySec * 10 &&
                            running_.load() && !reload_.load();
                 ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

// 1회 연결 → 패킷 수신 루프. 정상 종료(stop)든 오류든 자원 정리 후 반환.
bool RtspAvClient::openAndStream(const std::string& url) {
    // 인터럽트 콜백을 걸려면 컨텍스트를 직접 할당해 open에 넘겨야 한다.
    // (avformat_open_input은 실패 시 이 컨텍스트를 해제하고 포인터를 null로 만든다.)
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return false;
    fmt->interrupt_callback.callback = &RtspAvClient::interruptCb;
    fmt->interrupt_callback.opaque = this;

    // 1차: rtsp_transport=tcp 로 시도. 실패 시 옵션 없이 재시도(진단 겸 폴백).
    AVDictionary* opts = nullptr;
    setRtspOptions(&opts);
    int rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        char err[128] = {0};
        av_strerror(rc, err, sizeof(err));
        std::fprintf(stderr, "[ch%d] tcp 옵션 연결 실패 (%d: %s) — 옵션 없이 재시도\n",
                     channel_ + 1, rc, err);

        // 위 실패로 fmt는 이미 해제·null → 폴백용으로 다시 할당(인터럽트 콜백 재설정).
        fmt = avformat_alloc_context();
        if (!fmt) return false;
        fmt->interrupt_callback.callback = &RtspAvClient::interruptCb;
        fmt->interrupt_callback.opaque = this;
        rc = avformat_open_input(&fmt, url.c_str(), nullptr, nullptr);
        if (rc < 0) {
            char err2[128] = {0};
            av_strerror(rc, err2, sizeof(err2));
            std::fprintf(stderr, "[ch%d] RTSP 연결 실패 (%d: %s)\n",
                         channel_ + 1, rc, err2);
            if (fmt) avformat_close_input(&fmt);
            return false;
        }
        std::fprintf(stderr, "[ch%d] 옵션 없이 연결 성공 (기본 transport)\n", channel_ + 1);
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::fprintf(stderr, "[ch%d] 스트림 정보 조회 실패\n", channel_ + 1);
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
        std::fprintf(stderr, "[ch%d] 영상 트랙 없음\n", channel_ + 1);
        avformat_close_input(&fmt);
        return false;
    }

    // 영상 디코더 준비
    AVCodecParameters* vpar = fmt->streams[video_idx]->codecpar;
    {
        AVRational tb = fmt->streams[video_idx]->time_base;
        for (auto& cb : on_stream_readys_) cb(vpar, tb.num, tb.den);
    }
    const AVCodec* dec = avcodec_find_decoder(vpar->codec_id);
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dctx, vpar);
    // RPi 멀티코어 활용: 디코딩 스레드
    dctx->thread_count = 2;
    if (avcodec_open2(dctx, dec, nullptr) < 0) {
        std::fprintf(stderr, "[ch%d] 디코더 열기 실패\n", channel_ + 1);
        avcodec_free_context(&dctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;  // YUV→BGR 변환기 (첫 프레임에서 지연 생성)
    int sws_w = 0, sws_h = 0;
    // 화면 송출용 YUV→BGR + 축소(kViewWidth×kViewHeight) 변환기. 변환과 동시에
    // 축소해 파이프라인의 cv::resize를 대체한다 (병목 스레드에서 축소 부하 제거).
    SwsContext* sws_view = nullptr;
    int sws_view_w = 0, sws_view_h = 0;

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
                 channel_ + 1, video_idx, data_idx);

    bool ok = true;
    while (running_.load()) {
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            ok = false;  // 스트림 끊김 → 재연결
            break;
        }

        if (pkt->stream_index == video_idx) {
            // 압축 상태 그대로 블랙박스/NVR 등 소비자에게 전달 (디코딩 스로틀과 무관하게 전 패킷)
            for (auto& cb : on_packets_) cb(pkt);

            // ── 영상 패킷: 디코딩 → cv::Mat → 큐 ──
            if (avcodec_send_packet(dctx, pkt) == 0) {
                while (avcodec_receive_frame(dctx, frame) == 0) {
                    frame_count_.fetch_add(1);

                    // 소비 안 될 프레임은 BGR 변환 없이 버린다. 판정은 벽시계가 아니라
                    // 촬영시각(PTS) 기준 — 디코딩이 버스티하게 뭉쳐 나와도 촬영 간격이
                    // 정상(20fps=50ms)이면 통과시키고, 진짜 고fps만 걸러낸다.
                    int64_t gate_vts = frame->best_effort_timestamp;
                    if (gate_vts == AV_NOPTS_VALUE) gate_vts = frame->pts;
                    const auto gate_ts =
                        (gate_vts != AV_NOPTS_VALUE)
                            ? ptsToCapture(gate_vts * vtb_sec)
                            : std::chrono::steady_clock::now();
                    if (gate_ts - last_convert < convert_interval) continue;
                    last_convert = gate_ts;

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

                    // 화면 송출용 축소본을 같은 YUV에서 바로 뽑는다 (변환+축소 1패스).
                    // 원본이 화면 크기보다 클 때만 — 이하면 비워 두고 파이프라인이
                    // image로 폴백 처리(업스케일)한다.
                    cv::Mat view;
                    if (frame->width > kViewWidth || frame->height > kViewHeight) {
                        if (!sws_view || sws_view_w != frame->width ||
                            sws_view_h != frame->height) {
                            if (sws_view) sws_freeContext(sws_view);
                            sws_view = sws_getContext(
                                frame->width, frame->height,
                                static_cast<AVPixelFormat>(frame->format),
                                kViewWidth, kViewHeight, AV_PIX_FMT_BGR24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                            sws_view_w = frame->width;
                            sws_view_h = frame->height;
                        }
                        view.create(kViewHeight, kViewWidth, CV_8UC3);
                        uint8_t* vdst[1] = {view.data};
                        int vdst_stride[1] = {static_cast<int>(view.step)};
                        sws_scale(sws_view, frame->data, frame->linesize, 0,
                                  frame->height, vdst, vdst_stride);
                    }

                    // 촬영시각은 위 게이트에서 이미 계산(gate_ts) → 그대로 재사용.
                    queue_.push(Frame{channel_, std::move(img), gate_ts,
                                      std::move(view)});
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
    if (sws_view) sws_freeContext(sws_view);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dctx);
    avformat_close_input(&fmt);
    return ok;
}
