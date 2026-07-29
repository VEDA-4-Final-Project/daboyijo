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

//한 사람 ROI 분석
bool CaregiverDetector::isCaregiver(const cv::Mat& personROI) const {
    if (personROI.empty()) return false;

    cv::Rect torsoRect(0, 0, personROI.cols, personROI.rows);  // 상반신만 자름 -> 전체로 복귀
    cv::Mat torso = personROI(torsoRect);
    if (torso.empty()) return false;

    //색을 BGR -> HSV
    cv::Mat hsv;    
    cv::cvtColor(torso, hsv, cv::COLOR_BGR2HSV);

    //주황 범위에 드는 픽셀만 흰색, 나머지는 검정인 mask 생성
    cv::Mat mask;
    cv::inRange(hsv, lower_, upper_, mask);


    // 끊긴 조각 이어붙이기 — 주름·그림자로 갈라진 조끼를 한 덩어리로 복원
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, {5, 5}));

    // mask에서 흰 덩어리들의 바깥 외곽선을 찾아 contours에 담음 (점은 압축 저장)
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);


     // 전체 픽셀 수가 아니라 "가장 큰 덩어리" 하나만 — 흩어진 얼굴·손은 탈락
    double maxArea = 0.0;
    for (const auto& c : contours) {
        maxArea = std::max(maxArea, cv::contourArea(c));
    }

   // ratio (화면에서 주황색이 차지하는 비율)이 threshold_ 이상인지 판단
    double ratio = maxArea / (mask.rows * mask.cols);
    std::fprintf(stderr, "[caregiver] ratio=%.3f threshold=%.2f\n", ratio, threshold_);
    return ratio >= threshold_;
}

// 프레임 전체 순회
bool CaregiverDetector::detectInFrame(const cv::Mat& frame,
                                       const DetectionFrame& df) const {
    if (frame.empty()) return false;
    const int W = frame.cols;
    const int H = frame.rows;

    // [디버그] 이 프레임에 객체가 몇 개 들어왔는지
    // std::fprintf(stderr, "[detectInFrame] 객체 수=%zu\n", df.objects.size());


    for (const auto& obj : df.objects) {
        if (!obj.isHuman()) continue;         // df 중 Human 타입만 처리

        // [디버그] 사람 후보의 신뢰도
        // std::fprintf(stderr, "[detectInFrame] human likelihood=%.2f\n", obj.likelihood);
        //if (obj.likelihood < 0.5f) continue;  // 신뢰도 낮으면 스킵


        // 정규화 좌표(0~1) → 픽셀 Rect 변환
        int x = static_cast<int>(obj.left   * W);
        int y = static_cast<int>(obj.top    * H);
        int w = static_cast<int>(obj.width()  * W);
        int h = static_cast<int>(obj.height() * H);

        // 프레임 경계 클램핑 (좌표가 살짝 벗어나면 크래시 방지) -> ROi 박스 좌표
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