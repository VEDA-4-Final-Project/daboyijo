#include "sharpen_enhancer.hpp"

#include <algorithm>
#include <opencv2/imgproc.hpp>

SharpenEnhancer::SharpenEnhancer(double amount, double sigma)
    : amount_(amount), sigma_(sigma) {}

void SharpenEnhancer::process(int /*channel*/, cv::Mat& image,
                              const std::vector<Detection>& detections) {
    if (image.empty() || amount_ <= 0.0) return;

    for (const auto& d : detections) {
        // 오직 사람 박스만 선명하게. 얼굴(Head/Face)은 건드리지 않는다
        // — 그건 뒤따르는 블러 stage의 몫이다.
        if (d.type != "Human") continue;

        // 정규화 좌표(0.0~1.0) → 실제 픽셀 좌표 (privacy_masker와 동일 방식)
        int x0 = std::max(0, static_cast<int>(d.left * image.cols));
        int y0 = std::max(0, static_cast<int>(d.top * image.rows));
        int x1 = std::min(image.cols, static_cast<int>(d.right * image.cols));
        int y1 = std::min(image.rows, static_cast<int>(d.bottom * image.rows));

        cv::Rect roi(x0, y0, x1 - x0, y1 - y0);
        // 경계 밖·크기 0 예외 방지 (너무 작으면 샤프닝 의미 없음)
        if (roi.width < 3 || roi.height < 3) continue;

        // 사람 박스 영역만 잘라내 언샤프 마스킹:
        //   결과 = 원본 * (1 + amount) - 흐린것 * amount
        // 흐린 사본과의 차이(=윤곽)만큼 경계를 되살려 또렷하게 만든다.
        cv::Mat region = image(roi);  // image로의 뷰 — 여기에 쓰면 원본이 바뀜
        cv::Mat blurred;
        cv::GaussianBlur(region, blurred, cv::Size(0, 0), sigma_);
        cv::addWeighted(region, 1.0 + amount_, blurred, -amount_, 0.0, region);
    }
}
