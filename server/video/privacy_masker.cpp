#include "privacy_masker.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

PrivacyMasker::PrivacyMasker(int blur_kernel_size)
    : blur_kernel_size_(blur_kernel_size) {}

void PrivacyMasker::reportFall(int channel, int object_id, float cx, float cy) {
    std::lock_guard<std::mutex> lock(mutex_);
    fall_targets_[channel] = FallTarget{object_id, cx, cy};
}

// Qt/봇에서 확인 신호를 받으면 해당 채널의 낙상 상태를 해제 → 다시 전원 블러
void PrivacyMasker::clearFall(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    fall_targets_.erase(channel);
}

bool PrivacyMasker::hasFall(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fall_targets_.find(channel) != fall_targets_.end();
}

bool PrivacyMasker::computeFaceRoi(const Detection& d, const cv::Mat& image,
                                   cv::Rect& roi, cv::Mat& mask) {
    // 정규화 좌표(0.0~1.0)를 입력 이미지의 실제 픽셀 좌표로 변환
    int x0 = std::max(0, static_cast<int>(d.left * image.cols));
    int y0 = std::max(0, static_cast<int>(d.top * image.rows));
    int x1 = std::min(image.cols, static_cast<int>(d.right * image.cols));
    int y1 = std::min(image.rows, static_cast<int>(d.bottom * image.rows));
    // 움직임이 빠를때 노출 방지(패딩)
    int w = x1 - x0;
    int h = y1 - y0;
    int pad_x = static_cast<int>(w * 0.20);
    int pad_y = static_cast<int>(h * 0.20);

    int px0 = std::max(0, x0 - pad_x);
    int py0 = std::max(0, y0 - pad_y);
    int px1 = std::min(image.cols, x1 + pad_x);
    int py1 = std::min(image.rows, y1 + pad_y);

    roi = cv::Rect(px0, py0, px1 - px0, py1 - py0);
    if (roi.width <= 0 || roi.height <= 0) return false;

    // 얼굴 크기만한 까만 도화지 중심에 흰색(255) 타원(얼굴 형태) 그리기.
    // 패딩된 박스보다 타원을 살짝 작게(85%) 그려서 얼굴 윤곽에 더 밀착시킴.
    mask = cv::Mat::zeros(roi.size(), CV_8UC1);
    cv::Point center(roi.width / 2, roi.height / 2);
    cv::Size axes(static_cast<int>(roi.width / 2 * 0.85),
                  static_cast<int>(roi.height / 2 * 0.85));
    cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED);
    return true;
}

void PrivacyMasker::process(int channel, cv::Mat& image, const std::vector<Detection>& detections) {
    // WiseAI가 순간적으로 얼굴을 놓쳤을 때, 모자이크가 깜빡거리며 풀리는
    // 치명적인 프라이버시 사고를 막기 위해 마지막 좌표를 5프레임 유지
    struct HoldState {
        std::vector<Detection> last_dets;
        int remaining_frames = 0;
    };
    static std::map<int, HoldState> channel_hold_states;
    static std::mutex hold_mutex; // 멀티채널 스레드 세이프티 확보

    std::vector<Detection> active_dets = detections;

    {
        std::lock_guard<std::mutex> lock(hold_mutex);
        auto& state = channel_hold_states[channel];
        if (active_dets.empty()) {
            if (state.remaining_frames > 0) {
                active_dets = state.last_dets;
                state.remaining_frames--; // 수명 깎기
            }
        } else {
            state.last_dets = active_dets;
            state.remaining_frames = 5; // 새로운 좌표 발견 시 5프레임으로 수명 초기화
        }
    }

    // WiseAI 메타데이터 기반 얼굴 영역 블러 처리 (항상 전원 블러).
    // 낙상 중이더라도 여기서는 전원 블러본을 만든다 — 낙상자 노출은 별도로
    // restoreFallen()이 이 결과 위에 덧입힌다(Qt·보호자용). Gemini로 나가는
    // 스냅샷은 이 전원 블러본을 그대로 쓴다.
    for (const auto& d : active_dets) {
        if (d.type == "Head" || d.type == "Face") {
            cv::Rect face_roi;
            cv::Mat mask;
            if (!computeFaceRoi(d, image, face_roi, mask)) continue;

            // 얼굴 영역만 잘라내 블러 → 타원 마스크로 원본에 덮어쓰기
            cv::Mat roi_blur = image(face_roi).clone();
            int ksize = blur_kernel_size_;
            if (ksize % 2 == 0) ksize += 1; // 짝수 방지 안전장치
            cv::GaussianBlur(roi_blur, roi_blur, cv::Size(ksize, ksize), 20);
            roi_blur.copyTo(image(face_roi), mask);
        }
    }
}

void PrivacyMasker::restoreFallen(int channel, cv::Mat& image, const cv::Mat& clean,
                                  const std::vector<Detection>& detections) const {
    FallTarget target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fall_targets_.find(channel);
        if (it == fall_targets_.end()) return;  // 이 채널엔 낙상 없음
        target = it->second;
    }
    if (image.size() != clean.size()) return;  // 크기 다르면 좌표 불일치 — 안전 중단

    // 얼굴 하나를 clean(블러 전)에서 image(블러본)로 되살리는 로컬 헬퍼
    auto restore = [&](const Detection& d) {
        cv::Rect roi;
        cv::Mat mask;
        if (!computeFaceRoi(d, image, roi, mask)) return;
        clean(roi).copyTo(image(roi), mask);
    };

    // ── 1차: parent_id 매칭 ── 얼굴(Head/Face)의 parent_id가 낙상자 Human의
    // ObjectId를 가리키면 그 얼굴이 곧 낙상자 얼굴. 가장 확실한 신호.
    bool restored = false;
    for (const auto& d : detections) {
        if ((d.type == "Head" || d.type == "Face") &&
            d.parent_id == target.object_id) {
            restore(d);
            restored = true;
        }
    }
    if (restored) return;

    // ── 2차 폴백: 낙상자 Human bbox 안의 얼굴 ── WiseAI가 parent_id를 안 채운
    // 프레임 대비. 현재 프레임에서 같은 ObjectId의 Human을 찾아 그 박스 안에
    // 중심이 들어오는 얼굴을 노출.
    const Detection* fallen_human = nullptr;
    for (const auto& d : detections) {
        if (d.isHuman() && d.object_id == target.object_id) {
            fallen_human = &d;
            break;
        }
    }
    if (fallen_human) {
        for (const auto& d : detections) {
            if (d.type != "Head" && d.type != "Face") continue;
            float fx = (d.left + d.right) / 2.0f;
            float fy = (d.top + d.bottom) / 2.0f;
            if (fx >= fallen_human->left && fx <= fallen_human->right &&
                fy >= fallen_human->top && fy <= fallen_human->bottom) {
                restore(d);
                restored = true;
            }
        }
        if (restored) return;
    }

    // ── 3차 폴백: 저장해 둔 무게중심(cx,cy)에 가장 가까운 얼굴 ── 위 둘이 모두
    // 실패했을 때만. 임계 밖이면 아무것도 안 함(엉뚱한 사람 노출 방지, 전원 블러 유지).
    constexpr float kMaxDist = 0.25f;  // 정규화 거리 임계
    const Detection* best = nullptr;
    float best_dist = kMaxDist;
    for (const auto& d : detections) {
        if (d.type != "Head" && d.type != "Face") continue;
        float fx = (d.left + d.right) / 2.0f;
        float fy = (d.top + d.bottom) / 2.0f;
        float dist = std::hypot(fx - target.cx, fy - target.cy);
        if (dist < best_dist) {
            best_dist = dist;
            best = &d;
        }
    }
    if (best) restore(*best);
}
