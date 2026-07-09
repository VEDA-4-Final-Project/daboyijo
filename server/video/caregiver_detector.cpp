#include "caregiver_detector.hpp"

CaregiverDetector::CaregiverDetector(cv::Scalar lower, cv::Scalar upper, double threshold)
    : lower_(lower), upper_(upper), threshold_(threshold) {}

void CaregiverDetector::setColorRange(const cv::Scalar& lower, const cv::Scalar& upper) {
    lower_ = lower;
    upper_ = upper;
}

void CaregiverDetector::setThreshold(double threshold) {
    threshold_ = threshold;
}

bool CaregiverDetector::isCaregiver(const cv::Mat& personROI) const {
    if (personROI.empty()) return false;

    // 상반신(위쪽 절반)만 사용 — 상의 색에 집중
    cv::Rect torsoRect(0, 0, personROI.cols, personROI.rows / 2);
    cv::Mat torso = personROI(torsoRect);
    if (torso.empty()) return false;

    // BGR -> HSV 변환 (조명 변화에 강함)
    cv::Mat hsv;
    cv::cvtColor(torso, hsv, cv::COLOR_BGR2HSV);

    // 유니폼 색 범위 마스킹
    cv::Mat mask;
    cv::inRange(hsv, lower_, upper_, mask);

    // 노이즈 제거 (작은 점 제거)
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, {3, 3}));

    // 유니폼 색 픽셀 비율
    double ratio = static_cast<double>(cv::countNonZero(mask))
                 / (mask.rows * mask.cols);
    return ratio >= threshold_;
}