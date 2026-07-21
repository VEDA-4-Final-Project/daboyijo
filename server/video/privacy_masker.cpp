#include "privacy_masker.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <map>
#include <mutex>

PrivacyMasker::PrivacyMasker(int blur_kernel_size)
    : blur_kernel_size_(blur_kernel_size) {}

void PrivacyMasker::reportFall(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    fall_unmasked_[channel] = true;
}
// Qt에서 확인 신호를 받으면 해당 채널의 낙상 상태를 해제
void PrivacyMasker::clearFall(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    fall_unmasked_[channel] = false;
}

void PrivacyMasker::process(int channel, cv::Mat& image, const std::vector<Detection>& detections) {
    // 낙상 비상 상황 체크
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fall_unmasked_.find(channel);
        if (it != fall_unmasked_.end() && it->second == true) {
            return; // 수동으로 확인 버튼을 누르기 전까지 마스킹 작업을 건너뛰고 원본 송출
        }
    }

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

    // 평상시: WiseAI 메타데이터 기반 얼굴 영역 블러 처리
    for (const auto& d : active_dets) {
        if (d.type == "Head" || d.type == "Face") {
            // 정규화 좌표(0.0~1.0)를 입력 이미지의 실제 픽셀 좌표로 변환
            int x0 = std::max(0, static_cast<int>(d.left * image.cols));
            int y0 = std::max(0, static_cast<int>(d.top * image.rows));
            int x1 = std::min(image.cols, static_cast<int>(d.right * image.cols));
            int y1 = std::min(image.rows, static_cast<int>(d.bottom * image.rows));
            // 움직임이 빠를때 노출 방지(소량 패딩 — 얼굴에 밀착시키기 위해 축소)
            int w = x1 - x0;
            int h = y1 - y0;
            int pad_x = static_cast<int>(w * 0.10);
            int pad_y = static_cast<int>(h * 0.10);

            int px0 = std::max(0, x0 - pad_x);
            int py0 = std::max(0, y0 - pad_y);
            int px1 = std::min(image.cols, x1 + pad_x);
            int py1 = std::min(image.rows, y1 + pad_y);

            cv::Rect face_roi(px0, py0, px1 - px0, py1 - py0);

            // 이미지 경계 밖 예외 방지 및 유효성 검사
            if (face_roi.width > 0 && face_roi.height > 0) {
                // 1. 얼굴 영역만 싹둑 잘라내서 복사본을 만들고 블러 처리하기
                cv::Mat roi_blur = image(face_roi).clone();

                int ksize = blur_kernel_size_;
                if (ksize % 2 == 0) ksize += 1; // 짝수 방지 안전장치

                cv::GaussianBlur(roi_blur, roi_blur, 
                                 cv::Size(ksize, ksize), 20);

                // 2. 얼굴 크기만한 까만색 도화지(마스크) 만들기
                cv::Mat mask = cv::Mat::zeros(face_roi.size(), CV_8UC1);

                // 3. 까만 도화지 중심에 흰색(255)으로 꽉 찬 타원(얼굴 형태) 그리기
                // 패딩된 박스보다 타원을 살짝 작게(85%) 그려서 얼굴 윤곽에 더 밀착시킴
                cv::Point center(face_roi.width / 2, face_roi.height / 2);
                cv::Size axes(static_cast<int>(face_roi.width / 2 * 0.85),
                              static_cast<int>(face_roi.height / 2 * 0.85));
                cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED);

                // 4. 흰색 타원 영역만 원본 이미지 위에 싹 덮어씌우기 (마스킹 카피)
                roi_blur.copyTo(image(face_roi), mask);
            }
        }
    }
}