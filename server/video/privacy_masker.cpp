#include "privacy_masker.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>

PrivacyMasker::PrivacyMasker(double unmask_duration_sec, int blur_kernel_size)
    : unmask_duration_sec_(unmask_duration_sec), blur_kernel_size_(blur_kernel_size) {}

void PrivacyMasker::reportFall(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    fall_events_[channel] = std::chrono::steady_clock::now();
}

void PrivacyMasker::process(int channel, cv::Mat& image, const std::vector<Detection>& detections) {
    // 1. 낙상 비상 상황 체크 (안전 모드인 경우 블러 처리를 건너뜀)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fall_events_.count(channel)) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<double>(now - fall_events_[channel]).count();
            
            if (elapsed < unmask_duration_sec_) {
                return; // 10초 미만이면 마스킹을 하지 않고 원본 영상 송출
            }
        }
    }

    // 2. 평상시: WiseAI 메타데이터 기반 얼굴 영역 블러 처리
    for (const auto& d : detections) {
        if (d.type == "Head" || d.type == "Face") {
            // 정규화 좌표(0.0~1.0)를 입력 이미지의 실제 픽셀 좌표로 변환
            int x0 = std::max(0, static_cast<int>(d.left * image.cols));
            int y0 = std::max(0, static_cast<int>(d.top * image.rows));
            int x1 = std::min(image.cols, static_cast<int>(d.right * image.cols));
            int y1 = std::min(image.rows, static_cast<int>(d.bottom * image.rows));

            cv::Rect face_roi(x0, y0, x1 - x0, y1 - y0);

            // 이미지 경계 밖 예외 방지 및 유효성 검사
            if (face_roi.width > 0 && face_roi.height > 0) {
                cv::GaussianBlur(image(face_roi), image(face_roi), 
                                 cv::Size(blur_kernel_size_, blur_kernel_size_), 20);
            }
        }
    }
}