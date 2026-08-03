#include "fall_module.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

// ── 낙상감지 튜닝값 ──────────────────────────────────────────────
// Thunder(256x256)로 교체 — Lightning보다 느리지만(실측 100.8ms vs 36ms) 정확도가
// 높음. 우리 실부하(채널당 1~2명, 2초 간격)는 초당 2~4회 추론이면 되고 Thunder
// 예산(초당 9.9회, movenet_bench.py 실측)에 여유가 있어 시도해볼 만하다.
const std::string kPoseModelPath = "models/movenet_thunder_int8.tflite";
constexpr double kPoseIntervalSec = 2.0;
// 채널별 워커 구조에선 최악의 경우 4채널이 동시에 추론할 수 있다. 인스턴스당
// 2스레드면 동시 8스레드가 4코어를 다퉈(RTSP 디코딩·인코딩까지 포함) 오히려
// 전체가 느려지므로 1로 제한한다. 추론 1회가 느려지는 대신(실측 필요, 예상
// 2~3배) 객체당 2초 주기 예산 안에는 여전히 들어온다.
constexpr int kPoseNumThreads = 1;

cv::Rect normBoxToRect(float left, float top, float right, float bottom,
                       int imgW, int imgH) {
    constexpr float kCropMargin = 0.08f;
    const float w = right - left, h = bottom - top;
    left -= w * kCropMargin;
    right += w * kCropMargin;
    top -= h * kCropMargin;
    bottom += h * kCropMargin;

    const int x0 = std::max(0, static_cast<int>(left * imgW));
    const int y0 = std::max(0, static_cast<int>(top * imgH));
    const int x1 = std::min(imgW, static_cast<int>(right * imgW));
    const int y1 = std::min(imgH, static_cast<int>(bottom * imgH));
    return cv::Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

}  // namespace

void FallModule::setFallCallback(FallCallback cb) {
    fall_detector_.setFallCallback(std::move(cb));
}

void FallModule::addChannel(int channel) {
    if (channels_.count(channel)) return;  // 이미 등록됨
    auto state = std::make_unique<ChannelPose>(kPoseModelPath, kPoseNumThreads);
    if (!state->estimator.isReady()) {
        std::fprintf(stderr, "경고: [ch%d] MoveNet 로드 실패, 자세 판정 꺼짐\n",
                     channel + 1);
    }
    channels_[channel] = std::move(state);
}

void FallModule::updateBedRoi(int channel, bool clear,
                              std::vector<std::pair<float, float>> points) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clear) rois_.erase(channel);
    else rois_[channel] = std::move(points);
}

void FallModule::onMetadata(int channel, const std::vector<Detection>& dets) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<float, float>> roi;
    auto it = rois_.find(channel);
    if (it != rois_.end()) roi = it->second;
    fall_detector_.update(channel, dets, roi);
}

void FallModule::processFrame(const AiJob& job) {
    auto ch_it = channels_.find(job.channel);
    if (ch_it == channels_.end()) return;  // 미등록 채널
    ChannelPose& ch = *ch_it->second;
    if (!ch.estimator.isReady()) return;

    std::vector<FallDetector::ObservedTrack> observed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observed = fall_detector_.observedTracks(job.channel);
    }

    const auto pose_now = std::chrono::steady_clock::now();

    for (const auto& t : observed) {
        auto& last = ch.last_pose_time[t.object_id];
        if (std::chrono::duration<double>(pose_now - last).count() <
            kPoseIntervalSec) {
            continue;  // 객체당 kPoseIntervalSec 주기로 추론 제한
        }
        last = pose_now;

        // 원본 해상도에서 크롭 — 축소 프레임 크롭은 원거리 사람 자세 감지가
        // 불안정 (rtsp_av_client.cpp 실험 기록)
        cv::Rect roi = normBoxToRect(t.left, t.top, t.right, t.bottom,
                                     job.raw_frame.cols, job.raw_frame.rows);
        if (roi.width <= 0 || roi.height <= 0) continue;

        // 무거운 추론은 락 밖에서 — 메인 스트리밍·메타 콜백을 막지 않는다.
        // (저조도 보정은 PoseEstimator가 모델 입력 단계에서 처리 — pose_estimator.cpp)
        bool lying = ch.estimator.isLyingDown(job.raw_frame(roi));
        // std::fprintf(stderr, "[pose] ch%d obj%d 판정=%s (crop %dx%d)\n",
        //              job.channel + 1, t.object_id, lying ? "누움" : "서있음",
        //              roi.width, roi.height);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            fall_detector_.reportPose(job.channel, t.object_id, lying);
        }
    }
}
