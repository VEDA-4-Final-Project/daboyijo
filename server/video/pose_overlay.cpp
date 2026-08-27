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

// [지연 보정] 추론 당시 bbox 대비 현재 bbox의 크기 배율 허용 범위. WiseAI bbox는
// 사람이 잠깐 가려지거나 넘어지는 순간 크게 찌그러진다 — 그 배율을 그대로 관절에
// 먹이면 스켈레톤이 확 늘었다 줄었다 한다. 위치 보정은 살리고 크기 왜곡만 막는다.
constexpr float kMinRescale = 0.6f;
constexpr float kMaxRescale = 1.6f;

// MoveNet(COCO 17) 관절 연결 — 그릴 뼈대.
const int kBones[][2] = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4},              // 얼굴
    {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10},     // 어깨·팔
    {5, 11}, {6, 12}, {11, 12},                  // 몸통
    {11, 13}, {13, 15}, {12, 14}, {14, 16},      // 다리
};

}  // namespace

void PoseOverlay::put(int channel, int object_id, const Keypoints& kp_in_crop,
                      const cv::Rect2f& crop_norm, const cv::Rect2f& anchor_norm,
                      bool lying) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto& objs = channels_[channel];

    Entry& e = objs[object_id];
    e.object_id = object_id;
    e.kp = kp_in_crop;
    e.box = crop_norm;
    e.anchor = anchor_norm;
    e.lying = lying;
    e.at = now;

    for (auto it = objs.begin(); it != objs.end();) {
        const double idle =
            std::chrono::duration<double>(now - it->second.at).count();
        it = (idle > kPruneSec) ? objs.erase(it) : std::next(it);
    }
}

void PoseOverlay::draw(int channel, cv::Mat& image,
                       const std::vector<Detection>& dets) const {
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
        // ── [지연 보정] 관절 뭉치를 이 프레임의 사람 위치로 옮긴다 ──────────
        // 관절은 최대 한 추론 주기 전 사진의 것이지만, bbox는 매 프레임 최신이다
        // (속도 외삽까지 된 값). "추론 당시 bbox → 지금 bbox" 변환을 크롭 박스에
        // 그대로 먹이면 스켈레톤이 사람을 따라간다. 같은 object_id를 못 찾으면
        // (감지가 끊긴 프레임) 저장된 위치 그대로 — 이전 동작으로 안전 폴백.
        cv::Rect2f box = e.box;
        const Detection* cur = nullptr;
        for (const auto& d : dets) {
            if (d.isHuman() && d.object_id == e.object_id) { cur = &d; break; }
        }
        if (cur && e.anchor.width > 1e-6f && e.anchor.height > 1e-6f &&
            cur->width() > 1e-6f && cur->height() > 1e-6f) {
            const float sx = std::min(std::max(cur->width() / e.anchor.width,
                                               kMinRescale), kMaxRescale);
            const float sy = std::min(std::max(cur->height() / e.anchor.height,
                                               kMinRescale), kMaxRescale);
            box.x = cur->left + (e.box.x - e.anchor.x) * sx;
            box.y = cur->top + (e.box.y - e.anchor.y) * sy;
            box.width = e.box.width * sx;
            box.height = e.box.height * sy;
        }

        // 크롭 기준 0~1 → 프레임 픽셀
        cv::Point pt[PoseEstimator::kNumKeypoints];
        bool ok[PoseEstimator::kNumKeypoints];
        for (int i = 0; i < PoseEstimator::kNumKeypoints; ++i) {
            const auto& k = e.kp[i];
            ok[i] = k.score >= kMinDrawScore;
            pt[i] = cv::Point(
                static_cast<int>((box.x + k.x * box.width) * image.cols),
                static_cast<int>((box.y + k.y * box.height) * image.rows));
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

        // MoveNet에 넣은 크롭 영역. 평상시엔 글씨 없이 초록 스켈레톤만 두고,
        // 누움 판정일 때만 라벨을 띄운다 — 화면이 깔끔해야 점이 눈에 들어온다.
        // (한글은 OpenCV putText가 못 그려서 영문)
        const cv::Rect px(
            static_cast<int>(box.x * image.cols),
            static_cast<int>(box.y * image.rows),
            static_cast<int>(box.width * image.cols),
            static_cast<int>(box.height * image.rows));
        cv::rectangle(image, px, color, 1, cv::LINE_AA);
        if (e.lying) {
            cv::putText(image, "LYING",
                        cv::Point(px.x, std::max(12, px.y - 6)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 2, cv::LINE_AA);
        }
    }
}
