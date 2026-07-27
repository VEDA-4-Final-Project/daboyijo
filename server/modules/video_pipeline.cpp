#include "video_pipeline.hpp"

#include <chrono>
#include <map>
#include <utility>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 송출·가공(블러/인코딩)용 해상도. 캡처는 원본 해상도(예: 1280x720)로 들어오고
// AI는 raw 원본을 그대로 받으므로, 여기 값을 낮춰도 낙상 감지 정확도엔 영향이 없다.
// 감지 좌표는 정규화(0~1)라 블러도 이 해상도에 자동으로 맞춰진다.
// 960x540으로 낮춰 JPEG 인코딩 부하를 줄임(720p 대비 픽셀 56% → 인코딩 시간 ↓,
// 단일 파이프라인 스레드 처리량 ↑ → Qt 송출 fps 상승). raw.size()!=kViewSize면 resize.
const cv::Size kViewSize(960, 540);
const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 60};
// 프레임 레이트 방어선 — 너무 빨리 들어온 프레임은 버려서 발열·CPU 폭주 방지.
// 주의: 이 값을 입력 fps와 동률(예: 15fps 입력에 1/15)로 두면, 지터로 프레임이
// 조금만 빨리 와도 버려져 실효 fps가 절반 가까이 주저앉는다. 입력보다 넉넉히
// 위(여기선 20fps)로 잡아 정상 프레임은 통과시키고, 진짜 폭주만 막는다.
// (큐 입력은 RTSP 수신단 kMaxConvertFps=18이 이미 1차로 제한한다.)
constexpr double kMainProcessInterval = 1.0 / 20.0;  // 20fps 초과만 방어

// (테스트 후 싱크가 미세하게 안 맞으면 이 값을 늘리거나 줄여서 칼싱크 튜닝 가능!)
constexpr auto kDelayOffset = std::chrono::milliseconds(200);

}  // namespace

void VideoPipeline::run(const volatile std::sig_atomic_t& stop) {
    std::map<int, std::chrono::steady_clock::time_point> last_proc_time;

    while (!stop) {
        auto frame = queue_.pop(std::chrono::milliseconds(200));
        if (frame) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now -
                                              last_proc_time[frame->channel])
                    .count() < kMainProcessInterval) {
                continue;  // 15fps 초과분은 버림
            }
            last_proc_time[frame->channel] = now;

            const auto t0 = std::chrono::steady_clock::now();

            // 수신단은 원본 해상도 BGR로 보낸다 — MoveNet 크롭 해상도 확보용
            // (rtsp_av_client.cpp 상단 실험 기록 참조). GUI용 축소는 여기서.
            cv::Mat raw = std::move(frame->image);
            cv::Mat small;

            // INTER_NEAREST: 보간 없는 최근접 축소 — INTER_LINEAR 대비 3~4배 빠름.
            // 감시 송출용이라 미세한 계단현상은 감내(AI는 raw 원본을 따로 받음).
            if (raw.size() != kViewSize)
                cv::resize(raw, small, kViewSize, 0, 0, cv::INTER_NEAREST);
            else small = raw.clone();

            // AI 전달용 깨끗한 복사본 (블러 전 원본 — 낙상 선택본 복원 소스로도 씀)
            cv::Mat clean = small.clone();

            // resize+clone 소요만 따로 (전체 처리시간 중 준비 단계 비중 진단용)
            const double prep_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count();

            // 이 프레임의 생성 시각과 가장 궁합이 맞는 감지 좌표 선택
            auto dets = store_.closestTo(frame->channel, frame->received_at - kDelayOffset);

            // 송출 영상 가공 단계 실행 (블러 마스킹 등) → small = 전원 블러본
            for (auto& stage : stages_) {
                stage(frame->channel, small, dets);
            }

            // 낙상 중이면 "낙상자만 노출한 선택본" 생성 (전원 블러본 + clean 기반).
            // ai_.submit이 clean·dets를 move 하기 전에 만들어야 한다.
            cv::Mat selective;
            const bool has_selective =
                fall_variant_ &&
                fall_variant_(frame->channel, small, clean, dets, selective);

            // AI 워커에 최신 일감 던지기 (덮어쓰기 방식 — 밀림 방지)
            ai_.submit({std::move(raw), std::move(clean), frame->channel,
                        std::move(dets)});

            // imencode 순수 소요만 따로 누적 (전체 처리시간 중 인코딩 비중 진단용)
            double encode_ms = 0;

            // 전원 블러본은 항상 버퍼 A에 보관 → Gemini/외부로 나가는 스냅샷.
            std::vector<unsigned char> jpeg_full;
            const auto enc0 = std::chrono::steady_clock::now();
            cv::imencode(".jpg", small, jpeg_full, kJpegParams);
            encode_ms += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - enc0).count();
            snapshots_.put(frame->channel, jpeg_full, now);

            // Qt 송출·버퍼 B: 낙상 중이면 선택본, 아니면 전원 블러본.
            size_t bytes;
            if (has_selective) {
                std::vector<unsigned char> jpeg_sel;
                const auto enc1 = std::chrono::steady_clock::now();
                cv::imencode(".jpg", selective, jpeg_sel, kJpegParams);
                encode_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - enc1).count();
                // [보호자] 낙상 선택본 스냅샷 보관 (텔레그램 낙상 사진용).
                snapshots_fall_.put(frame->channel, jpeg_sel, now);
                bytes = jpeg_sel.size();
                server_.broadcast(frame->channel, std::move(jpeg_sel));
            } else {
                // 평상시: 전원 블러본을 복사 없이 그대로 송출 (버퍼 A엔 이미 put 완료).
                bytes = jpeg_full.size();
                server_.broadcast(frame->channel, std::move(jpeg_full));
            }

            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            stats_.onFrameSent(frame->channel, bytes, ms, prep_ms, encode_ms);
        }

        stats_.maybeReport();
    }
}
