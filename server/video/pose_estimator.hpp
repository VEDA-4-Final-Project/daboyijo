#pragma once

#include <array>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

// MoveNet SinglePose(Lightning, int8) 추론 래퍼.
//
// WiseAI가 준 사람 bbox로 크롭한 이미지 하나를 넣으면 관절 17개를 얻고,
// 어깨-엉덩이 벡터의 기울기로 "누워있는지"를 판정한다. 방향(정면/측면)과
// 무관하게 동작해 무게중심·머리 cy 같은 "속도" 기반 신호의 사각지대를 메운다
// — 노인 낙상은 천천히 주저앉는 경우가 많아 급강하 속도로는 잘 안 잡혔다
// (docs/wiseai-메타데이터-명세.md, fall_detector.hpp 참조).
//
// 헤더에 TFLite 타입을 노출하지 않는다(pimpl) — 이 파일을 include하는 쪽은
// TFLite 헤더/빌드 경로를 몰라도 된다. 실제 TFLite 연동은 .cpp에서만 함.
class PoseEstimator {
public:
    // modelPath: MoveNet SinglePose Lightning int8 .tflite 경로.
    // 로드 실패해도 예외를 던지지 않는다 — isReady()로 확인할 것.
    // 실패 상태에선 isLyingDown()이 항상 false(=낙상 아님으로 보수적 처리)를 반환한다.
    explicit PoseEstimator(const std::string& modelPath, int numThreads = 4);
    ~PoseEstimator();

    PoseEstimator(const PoseEstimator&) = delete;
    PoseEstimator& operator=(const PoseEstimator&) = delete;

    bool isReady() const;

    // 관절 1개: 정규화 좌표(입력 크롭 기준 0~1) + 신뢰도(0~1).
    struct Keypoint {
        float y = 0, x = 0, score = 0;
    };
    static constexpr int kNumKeypoints = 17;
    // MoveNet(COCO) 관절 순서
    enum : int {
        kNose = 0, kLeftEye, kRightEye, kLeftEar, kRightEar,
        kLeftShoulder, kRightShoulder, kLeftElbow, kRightElbow,
        kLeftWrist, kRightWrist, kLeftHip, kRightHip,
        kLeftKnee, kRightKnee, kLeftAnkle, kRightAnkle,
    };

    // 크롭 이미지(BGR, 임의 크기)에서 관절 17개를 추정한다.
    // 실패(모델 미준비·빈 이미지·추론 오류) 시 false, out은 건드리지 않음.
    bool estimate(const cv::Mat& personCropBgr,
                  std::array<Keypoint, kNumKeypoints>& out) const;

    // estimate() 결과로 "누운 자세"인지 판정한다. 세 신호의 OR — 하나라도
    // 걸리면 누움 (임계값은 pose_estimator.cpp 상단, 실측 전 잠정값):
    //   ① 몸통 기울기: 어깨중점→엉덩이중점 벡터가 수직에서 kLyingAngleDeg
    //      이상 기울면 누움. 옆으로 눕는 낙상 담당.
    //   ② 상하 반전: 어깨가 엉덩이보다 화면 아래면 누움. 머리부터 고꾸라진
    //      자세 담당 (①은 절댓값 각도라 반전을 못 본다).
    //   ③ 원근 단축: 세로 스프레드(어깨~발목/무릎) ÷ 어깨 너비가 기준 미만이면
    //      누움. 카메라 축 방향으로 눕는 낙상 담당 — 몸통은 투영에서 짧아지지만
    //      어깨 너비는 그대로라는 성질을 이용 (①의 사각지대, 실측으로 확인됨).
    // 어깨·엉덩이 중 신뢰도 낮은 관절이 있으면(가림 등) 판정 보류(false).
    bool isLyingPose(const std::array<Keypoint, kNumKeypoints>& kp) const;

    // estimate() + isLyingPose() 를 한 번에 수행하는 편의 함수.
    bool isLyingDown(const cv::Mat& personCropBgr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
