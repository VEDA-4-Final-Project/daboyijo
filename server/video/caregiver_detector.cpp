#include "caregiver_detector.hpp"
#include <algorithm>   // std::max, std::min
#include <cstdio>
#include <vector>

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

    // [디버그] 상반신 영역의 실제 HSV 통계 — 색 범위 튜닝용.
    // 출력뿐 아니라 통계 계산 자체가 사람당 매번 돌던 낭비라 통째로 비활성화.
    // 색 범위 다시 튜닝할 때 #if 1로 켜서 쓸 것.
#if 0
    {
        cv::Scalar meanHsv, stdHsv;
        cv::meanStdDev(hsv, meanHsv, stdHsv);   // 채널별 평균/표준편차

        // 채널 분리해서 최소/최대도 확인
        std::vector<cv::Mat> ch;
        cv::split(hsv, ch);
        double hMin, hMax, sMin, sMax, vMin, vMax;
        cv::minMaxLoc(ch[0], &hMin, &hMax);
        cv::minMaxLoc(ch[1], &sMin, &sMax);
        cv::minMaxLoc(ch[2], &vMin, &vMax);

        // std::fprintf(stderr,
        //     "[caregiver] HSV 평균 H=%.1f S=%.1f V=%.1f | 편차 H=%.1f S=%.1f V=%.1f\n",
        //     meanHsv[0], meanHsv[1], meanHsv[2],
        //     stdHsv[0], stdHsv[1], stdHsv[2]);
        // std::fprintf(stderr,
        //     "[caregiver] HSV 범위  H=%.0f~%.0f S=%.0f~%.0f V=%.0f~%.0f\n",
        //     hMin, hMax, sMin, sMax, vMin, vMax);
        // std::fprintf(stderr,
        //     "[caregiver] 현재 설정 lower=(%.0f,%.0f,%.0f) upper=(%.0f,%.0f,%.0f)\n",
        //     lower_[0], lower_[1], lower_[2], upper_[0], upper_[1], upper_[2]);
    }
#endif

    std::fprintf(stderr, "[caregiver] ratio=%.3f threshold=%.2f\n", ratio, threshold_);

    return ratio >= threshold_;
}

bool CaregiverDetector::detectInFrame(const cv::Mat& frame,
                                       const DetectionFrame& df) const {
    if (frame.empty()) return false;
    const int W = frame.cols;
    const int H = frame.rows;

    for (const auto& obj : df.objects) {
        if (!obj.isHuman()) continue;         // Human 타입만
        if (obj.likelihood < 0.5f) continue;  // 신뢰도 낮으면 스킵

        // 정규화 좌표(0~1) → 픽셀 Rect 변환
        int x = static_cast<int>(obj.left   * W);
        int y = static_cast<int>(obj.top    * H);
        int w = static_cast<int>(obj.width()  * W);
        int h = static_cast<int>(obj.height() * H);

        // 프레임 경계 클램핑 (좌표가 살짝 벗어나면 크래시 방지)
        x = std::max(0, x);
        y = std::max(0, y);
        w = std::min(w, W - x);
        h = std::min(h, H - y);
        if (w <= 0 || h <= 0) continue;

        cv::Rect box(x, y, w, h);
        if (isCaregiver(frame(box))) return true;   // 보호사 하나라도 있으면 true
    }
    return false;
}