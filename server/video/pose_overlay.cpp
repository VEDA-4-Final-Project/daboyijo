#include "pose_overlay.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

// 이 신뢰도 미만의 관절은 그리지 않는다 — 가려진 팔다리가 엉뚱한 곳에 찍히면
// "AI가 헛것을 본다"로 보인다. 판정(isLyingPose)의 기준(0.25)보다 살짝 낮게 잡아
// 판정에 쓰인 관절은 화면에도 전부 보이게 한다.
constexpr float kMinDrawScore = 0.20f;

// 마지막 추론이 이보다 오래됐으면 그리지 않는다. 추론 주기(pose_interval_sec)
// 보다 넉넉해야 점이 깜빡이지 않고, 사람이 사라진 뒤에는 곧 지워질 만큼 짧아야 한다.
constexpr double kStaleSec = 1.5;

// 이보다 오래된 항목은 put() 때 정리 (객체가 사라져도 map이 안 불어나게).
constexpr double kPruneSec = 10.0;

// MoveNet(COCO 17) 관절 연결 — 그릴 뼈대.
const int kBones[][2] = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4},              // 얼굴
    {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10},     // 어깨·팔
    {5, 11}, {6, 12}, {11, 12},                  // 몸통
    {11, 13}, {13, 15}, {12, 14}, {14, 16},      // 다리
};

}  // namespace

void PoseOverlay::put(int channel, int object_id, const Keypoints& kp_in_crop,
                      const cv::Rect2f& crop_norm, bool lying) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto& objs = channels_[channel];

    Entry& e = objs[object_id];
    e.kp = kp_in_crop;
    e.box = crop_norm;
    e.lying = lying;
    e.at = now;

    for (auto it = objs.begin(); it != objs.end();) {
        const double idle =
            std::chrono::duration<double>(now - it->second.at).count();
        it = (idle > kPruneSec) ? objs.erase(it) : std::next(it);
    }
}

void PoseOverlay::draw(int channel, cv::Mat& image) const {
    if (image.empty()) return;

    // 락은 복사까지만 — 그리기(수십 μs)는 락 밖에서 한다.
    std::vector<Entry> fresh;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        auto ch = channels_.find(channel);
        if (ch == channels_.end()) return;
        for (const auto& kv : ch->second) {
            if (std::chrono::duration<double>(now - kv.second.at).count() <=
                kStaleSec) {
                fresh.push_back(kv.second);
            }
        }
    }

    for (const auto& e : fresh) {
        // 크롭 기준 0~1 → 프레임 픽셀
        cv::Point pt[PoseEstimator::kNumKeypoints];
        bool ok[PoseEstimator::kNumKeypoints];
        for (int i = 0; i < PoseEstimator::kNumKeypoints; ++i) {
            const auto& k = e.kp[i];
            ok[i] = k.score >= kMinDrawScore;
            pt[i] = cv::Point(
                static_cast<int>((e.box.x + k.x * e.box.width) * image.cols),
                static_cast<int>((e.box.y + k.y * e.box.height) * image.rows));
        }

        // 누움이면 빨강, 아니면 초록 (판정 결과가 색으로 바로 보인다)
        const cv::Scalar color = e.lying ? cv::Scalar(0, 0, 255)
                                         : cv::Scalar(0, 255, 0);

        for (const auto& b : kBones) {
            if (ok[b[0]] && ok[b[1]]) {
                cv::line(image, pt[b[0]], pt[b[1]], color, 2, cv::LINE_AA);
            }
        }
        for (int i = 0; i < PoseEstimator::kNumKeypoints; ++i) {
            if (!ok[i]) continue;
            // 검은 테두리 + 노란 점 — 밝은 배경에서도 점이 묻히지 않게.
            cv::circle(image, pt[i], 4, cv::Scalar(0, 0, 0), -1, cv::LINE_AA);
            cv::circle(image, pt[i], 3, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);
        }

        // MoveNet에 넣은 크롭 영역 + 상태 라벨 (한글은 OpenCV putText가 못 그린다)
        const cv::Rect box(
            static_cast<int>(e.box.x * image.cols),
            static_cast<int>(e.box.y * image.rows),
            static_cast<int>(e.box.width * image.cols),
            static_cast<int>(e.box.height * image.rows));
        cv::rectangle(image, box, color, 1, cv::LINE_AA);
        cv::putText(image, e.lying ? "MoveNet: LYING" : "MoveNet: UPRIGHT",
                    cv::Point(box.x, std::max(12, box.y - 6)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv::LINE_AA);
    }
}
