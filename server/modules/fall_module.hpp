#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "ai_worker.hpp"
#include "detection.hpp"
#include "fall_detector.hpp"
#include "pose_estimator.hpp"

// ══ [낙상감지] 모듈 — 담당자는 이 파일과 core/fall_detector.*,
//    video/pose_estimator.* 만 수정하면 된다 ══
//
// FallDetector(침대 ROI 게이팅·낙상 확정)와 PoseEstimator(MoveNet 자세 판정)를
// 묶어 배선한다. 판정 흐름은 fall_detector.hpp 상단 주석 참조.
// 모델 경로·추론 주기 같은 튜닝값은 fall_module.cpp 상단에 있다.
class FallModule {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    FallModule();

    // 낙상 확정 시 1회 호출될 콜백 (main.cpp에서 블러 해제·블랙박스·경보로 배선)
    void setFallCallback(FallCallback cb);

    // Qt가 그린 침대 ROI 갱신 (StreamServer ROI 콜백에서 호출)
    void updateBedRoi(int channel, bool clear,
                      std::vector<std::pair<float, float>> points);

    // RTSP 메타데이터 콜백마다 호출 — ROI 게이팅 + bbox 캐시 갱신
    void onMetadata(int channel, const std::vector<Detection>& dets);

    // AiWorker에 등록할 프로세서 — 관찰 대상(침대 밖 사람) bbox 크롭 →
    // MoveNet "누움" 판정 → FallDetector에 보고 (지속되면 낙상 확정 콜백)
    void processFrame(const AiJob& job);

private:
    // fall_detector_·rois_ 보호 (RTSP 수신 스레드들 + AI 워커 스레드 공유)
    std::mutex mutex_;
    FallDetector fall_detector_;
    std::map<int, std::vector<std::pair<float, float>>> rois_;

    PoseEstimator pose_estimator_;
    // 채널 → (객체 → 마지막 추론 시각). AI 워커 스레드 전용이라 락 불필요.
    std::map<int, std::map<int, std::chrono::steady_clock::time_point>>
        last_pose_time_;
};
