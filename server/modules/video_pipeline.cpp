#include "video_pipeline.hpp"

#include <chrono>
#include <map>
#include <utility>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 캡처·송출 모두 1280x720로 통일 → raw.size()==kViewSize면 리사이즈 없이 clone.
const cv::Size kViewSize(1280, 720);
const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};
// 프레임 레이트 방어선 — 너무 빨리 들어온 프레임은 버려서 발열·CPU 폭주 방지
constexpr double kMainProcessInterval = 1.0 / 15.0;  // 최대 15fps

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

            if (raw.size() != kViewSize) cv::resize(raw, small, kViewSize);
            else small = raw.clone();

            // AI 전달용 깨끗한 복사본
            cv::Mat clean = small.clone();

            // 이 프레임의 생성 시각과 가장 궁합이 맞는 감지 좌표 선택
            auto dets = store_.closestTo(frame->channel, frame->received_at - kDelayOffset);

            // 송출 영상 가공 단계 실행 (블러 마스킹 등)
            for (auto& stage : stages_) {
                stage(frame->channel, small, dets);
            }

            // AI 워커에 최신 일감 던지기 (덮어쓰기 방식 — 밀림 방지)
            ai_.submit({std::move(raw), std::move(clean), frame->channel,
                        std::move(dets)});

            // 가공 완료된 이미지를 인코딩해서 Qt로 송출
            std::vector<unsigned char> jpeg;
            cv::imencode(".jpg", small, jpeg, kJpegParams);
            const size_t bytes = jpeg.size();
            // [케어봇] 봇 스냅샷용으로 최신 JPEG 1장 보관 (블러가 이미 적용된 프레임).
            snapshots_.put(frame->channel, jpeg, now);
            server_.broadcast(frame->channel, std::move(jpeg));

            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            stats_.onFrameSent(frame->channel, bytes, ms);
        }

        stats_.maybeReport();
    }
}
