#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
//
// ★ AiWorker가 채널별 전담 스레드 구조라서, 자세 추정기(TFLite 인터프리터는
//   동시 호출 불가)도 채널별 인스턴스로 분리했다. 채널별 상태는 그 채널의
//   워커 스레드만 만지므로 락이 필요 없고, 채널 간 공유인 fall_detector_와
//   rois_만 mutex_로 보호한다.
class FallModule {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    // 자세 추론 1회마다(객체당 ~2fps) 호출 — 관절 17개를 "프레임 전체 기준"
    // 정규화 0~1 좌표로 넘긴다. main.cpp가 StreamServer::broadcastPose로 배선해
    // Qt에서 스켈레톤을 그린다(낙상 판정과 무관한 시각화용). 누움/서있음 모두 보고.
    using PoseCallback = std::function<void(
        int channel, int object_id,
        const std::array<PoseEstimator::Keypoint, PoseEstimator::kNumKeypoints>& framePts)>;

    // 낙상 확정 시 1회 호출될 콜백 (main.cpp에서 블러 해제·블랙박스·경보로 배선)
    void setFallCallback(FallCallback cb);

    // 자세 시각화 콜백 등록 (선택 — 미등록이면 스켈레톤 송출 안 함).
    void setPoseCallback(PoseCallback cb) { on_pose_ = std::move(cb); }

    // 채널 등록 — 채널 전용 MoveNet 인스턴스를 로드한다.
    // AiWorker start 전, 카메라 루프에서 호출할 것.
    void addChannel(int channel);

    // Qt가 그린 침대 ROI 갱신 (StreamServer ROI 콜백에서 호출)
    void updateBedRoi(int channel, bool clear,
                      std::vector<std::pair<float, float>> points);

    // RTSP 메타데이터 콜백마다 호출 — ROI 게이팅 + bbox 캐시 갱신
    void onMetadata(int channel, const std::vector<Detection>& dets);

    // AiWorker에 등록할 프로세서 — 관찰 대상(침대 밖 사람) bbox 크롭 →
    // MoveNet "누움" 판정 → FallDetector에 보고 (지속되면 낙상 확정 콜백)
    void processFrame(const AiJob& job);

private:
    // 채널 하나의 자세 추정 상태 — 해당 채널의 AI 워커 스레드 전용 (락 불필요)
    struct ChannelPose {
        ChannelPose(const std::string& model_path, int num_threads)
            : estimator(model_path, num_threads) {}
        PoseEstimator estimator;
        // 객체 → 마지막 추론 시각 (객체당 추론 주기 제한용)
        std::map<int, std::chrono::steady_clock::time_point> last_pose_time;
    };

    // fall_detector_·rois_ 보호 (RTSP 수신 스레드들 + 채널별 AI 워커 스레드 공유)
    std::mutex mutex_;
    FallDetector fall_detector_;
    std::map<int, std::vector<std::pair<float, float>>> rois_;
    PoseCallback on_pose_;  // 자세 시각화 콜백 (선택)

    // 채널 → 자세 추정 상태. addChannel은 메인 스레드에서 AI 워커 시작 전에만
    // 호출되고, 이후 각 항목은 그 채널의 워커 스레드 전용이라 락 불필요.
    std::map<int, std::unique_ptr<ChannelPose>> channels_;
};
