#ifndef CAREGIVER_DETECTOR_HPP
#define CAREGIVER_DETECTOR_HPP

#include "detection.hpp"
#include <opencv2/opencv.hpp>

// 옷 색(HSV) 기반 보호사 판정기
class CaregiverDetector {
public:
    // HSV 하한/상한과 판정 임계값(유니폼 색 픽셀 비율)
    // 아래 값들은 단독 테스트용 fallback.
    // 실제 동작값은 modules/caregiver_module.cpp 상단 상수가 주입한다.
    CaregiverDetector(cv::Scalar lower = cv::Scalar(5, 50, 50),
                      cv::Scalar upper = cv::Scalar(28, 255, 255),
                      double threshold = 0.08);

    // 사람 영역(personROI)이 보호사인지 판정
    bool isCaregiver(const cv::Mat& personROI) const;

    // DetectionFrame(사람 검출 결과) 받아서 보호사 있는지 판정
    bool detectInFrame(const cv::Mat& frame, const DetectionFrame& df) const;

    // 튜닝용 — 색 범위/임계값 조정
    void setColorRange(const cv::Scalar& lower, const cv::Scalar& upper);
    void setThreshold(double threshold);

    // 튜닝용 — 최소 평균 채도 조정 (caregiver_module.cpp에서 주입)
    void setMinSaturation(double s) { min_sat_ = s; }

private:
    cv::Scalar lower_;
    cv::Scalar upper_;
    double threshold_;
    //   조끼 판정 최소 평균 채도. 실측상 조끼는 S 150~207,
    //   피부·사복·나무는 S 55~137로 갈려서 이 값이 주 판정 역할을 한다.
    double min_sat_ = 150.0;
};

#endif // CAREGIVER_DETECTOR_HPP