#ifndef CAREGIVER_DETECTOR_HPP
#define CAREGIVER_DETECTOR_HPP

#include "detection.hpp"
#include <opencv2/opencv.hpp>
#include <chrono>

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

    // DetectionFrame(사람 검출 결과) 받아서 보호사 있는지 판정.
    // caregiver_ids가 널이 아니면 보호사로 판정된 객체의 ObjectId를 거기에 담는다
    // (IdentityTracker가 "이 트랙은 보호사니 침대에 귀속하지 말 것"으로 쓴다).
    // ★ 널이면 첫 보호사를 찾는 즉시 멈춘다(케어 타이머는 존재 여부만 필요) —
    //   목록을 달라고 하면 사람 수만큼 HSV 분석이 돌므로 필요할 때만 넘길 것.
    bool detectInFrame(const cv::Mat& frame, const DetectionFrame& df,
                       std::vector<int>* caregiver_ids = nullptr) const;

    // 튜닝용 — 색 범위/임계값 조정
    void setColorRange(const cv::Scalar& lower, const cv::Scalar& upper);
    void setThreshold(double threshold);

    // 튜닝용 — 최소 평균 채도 조정 (caregiver_module.cpp에서 주입)
    void setMinSaturation(double s) { min_sat_ = s; }

private:
    cv::Scalar lower_;
    cv::Scalar upper_;
    double threshold_;
    //   유니폼 판정 최소 평균 채도 — 주 판정. 유니폼(주황 긴팔)은 채도가 높게
    //   깔리고 피부·사복·나무는 낮게 깔려서 이 축이 가장 잘 갈린다.
    //   ★ 실제 동작값과 그 근거(조끼 → 긴팔 전환 이력)는
    //     modules/caregiver_module.cpp 상단에 있다. 여기 값은 단독 테스트용.
    double min_sat_ = 150.0;

    // 디버그 로그 스로틀 — 프레임마다 쏟아지지 않게 0.5초에 한 번만 출력
    mutable std::chrono::steady_clock::time_point last_log_time_{};
};

#endif // CAREGIVER_DETECTOR_HPP