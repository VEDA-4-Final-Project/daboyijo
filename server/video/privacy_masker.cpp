#include "privacy_masker.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>

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
    // 1. 낙상 비상 상황 체크
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fall_unmasked_.find(channel);
        if (it != fall_unmasked_.end() && it->second == true) {
            return; // 수동으로 확인 버튼을 누르기 전까지 마스킹 작업을 건너뛰고 원본 송출
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
                // 1. 얼굴 영역만 싹둑 잘라내서 복사본을 만들고 블러 처리하기
                cv::Mat roi_blur = image(face_roi).clone();

                int ksize = blur_kernel_size_;
                if (ksize % 2 == 0) ksize += 1; // 짝수 방지 안전장치

                cv::GaussianBlur(roi_blur, roi_blur, 
                                 cv::Size(ksize, ksize), 20);

                // 2. 얼굴 크기만한 까만색 도화지(마스크) 만들기
                cv::Mat mask = cv::Mat::zeros(face_roi.size(), CV_8UC1);

                // 3. 까만 도화지 중심에 흰색(255)으로 꽉 찬 타원(얼굴 형태) 그리기
                cv::Point center(face_roi.width / 2, face_roi.height / 2);
                cv::Size axes(face_roi.width / 2, face_roi.height / 2);
                cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED);

                // 4. [핵심] 흰색 타원 영역만 원본 이미지 위에 싹 덮어씌우기 (마스킹 카피)
                roi_blur.copyTo(image(face_roi), mask);
            }
        }
    }
}