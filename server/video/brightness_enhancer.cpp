#include "brightness_enhancer.hpp"

#include <cmath>

#include <opencv2/core.hpp>

BrightnessEnhancer::BrightnessEnhancer(double gamma) {
    // gamma가 1에 매우 가까우면 효과가 없으니 stage 자체를 건너뛴다.
    enabled_ = std::fabs(gamma - 1.0) > 1e-3;
    if (!enabled_) return;

    // 감마 보정 LUT:  out = 255 * (in/255)^(1/gamma)
    //   gamma>1 → 지수(1/gamma)<1 → 어두운 값이 크게 올라가 전체가 밝아진다.
    //   한 번만 계산해 두면 프레임마다는 테이블 룩업(cv::LUT)뿐이라 매우 싸다.
    const double inv = 1.0 / gamma;
    lut_.create(1, 256, CV_8UC1);
    uchar* p = lut_.ptr<uchar>();
    for (int i = 0; i < 256; ++i) {
        double v = std::pow(i / 255.0, inv) * 255.0 + 0.5;  // +0.5: 반올림
        p[i] = static_cast<uchar>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
    }
}

void BrightnessEnhancer::process(int /*channel*/, cv::Mat& image,
                                 const std::vector<Detection>& /*detections*/) {
    if (!enabled_ || image.empty()) return;
    // 동일 LUT를 BGR 각 채널에 균등 적용 → 밝기만 올리고 색균형은 유지.
    // cv::LUT는 in-place(src==dst)를 지원한다.
    cv::LUT(image, lut_, image);
}
