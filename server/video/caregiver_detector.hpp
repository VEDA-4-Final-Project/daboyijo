#ifndef CAREGIVER_DETECTOR_HPP
#define CAREGIVER_DETECTOR_HPP

#include "detection.hpp"
#include <opencv2/opencv.hpp>

// 옷 색(HSV) 기반 보호사 판정기
class CaregiverDetector {
public:
    // HSV 하한/상한과 판정 임계값(유니폼 색 픽셀 비율)
    CaregiverDetector(cv::Scalar lower = cv::Scalar(100, 80, 40),
                      cv::Scalar upper = cv::Scalar(130, 255, 255),
                      double threshold = 0.35);

    // 사람 영역(personROI)이 보호사인지 판정
    bool isCaregiver(const cv::Mat& personROI) const;

    // DetectionFrame(사람 검출 결과) 받아서 보호사 있는지 판정
    bool detectInFrame(const cv::Mat& frame, const DetectionFrame& df) const;

    // 튜닝용 — 색 범위/임계값 조정
    void setColorRange(const cv::Scalar& lower, const cv::Scalar& upper);
    void setThreshold(double threshold);

private:
    cv::Scalar lower_;
    cv::Scalar upper_;
    double threshold_;
};

#endif // CAREGIVER_DETECTOR_HPP