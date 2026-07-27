#include "video_pipeline.hpp"

#include <chrono>
#include <map>
#include <utility>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 송출·가공(블러/인코딩)용 해상도. frame_queue.hpp의 kViewWidth/Height와 동일.
// 축소는 디코딩 스레드의 sws_scale이 미리 해 두므로(frame->view), 여기선 그걸
// 그대로 쓴다. view가 비어 있을 때만 폴백으로 cv::resize.
const cv::Size kViewSize(kViewWidth, kViewHeight);
const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};
// 프레임 레이트 방어선 — 너무 빨리 들어온 프레임은 버려서 발열·CPU 폭주 방지.
// 주의: 이 값을 입력 fps에 가깝게 두면, 지터로 프레임이 조금만 빨리 와도 버려져
// 실효 fps가 주저앉는다. 입력(15fps=66.7ms)보다 넉넉히 위(30fps=33ms)로 잡아
// 정상 프레임은 지터가 있어도 다 통과시키고, 진짜 폭주만 막는다. 발열/CPU 상한은
// RTSP 수신단 kMaxConvertFps=18이 큐 입력 단계에서 이미 1차로 눌러 준다.
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

            // 축소본은 디코딩 스레드가 sws_scale로 미리 만들어 둔다 → 여기선 이동만
            // (병목 스레드에서 resize 부하 제거). view가 없는 경우에만 폴백 resize.
            if (!frame->view.empty()) small = std::move(frame->view);
            else if (raw.size() != kViewSize) cv::resize(raw, small, kViewSize);
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
