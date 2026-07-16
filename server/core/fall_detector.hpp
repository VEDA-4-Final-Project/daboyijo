#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "detection.hpp"

// 침대 ROI 게이팅 + MoveNet 자세 추정 기반 낙상 판정기.
//
// 판정 흐름:
//   ⓪ 침대 ROI 안이면 전부 무시(취침·뒤척임). ROI 밖이면 "관찰 대상".
//   ① main.cpp가 매 비디오 프레임마다 observedTracks()로 관찰 대상(침대 밖
//      사람)의 bbox 목록을 받아, WiseAI bbox로 크롭한 이미지를
//      PoseEstimator(MoveNet)에 돌려 "누운 자세"인지 얻고 reportPose()로
//      알려준다 (사람 1명당 약 2fps로 제한 권장 — server/tools/movenet_bench.py
//      실측 기준 RPi4에서 초당 28회 예산).
//   ② 누운 자세가 kLyingConfirmSec 동안 연속으로 유지되면 낙상 확정.
//      (한두 프레임의 오분류로 바로 알람이 뜨지 않도록 지속시간으로 필터링)
//
// 이전 버전은 무게중심/머리 cy의 "급강하 속도"를 트리거로 썼으나, 노인 낙상은
// 천천히 주저앉는 경우가 많아 속도 기반 트리거가 잘 안 걸리는 문제가 있었다.
// MoveNet 몸통 각도(어깨↔엉덩이 벡터의 기울기)는 속도·방향과 무관하게
// "지금 누워있는가" 자체를 직접 보므로 이 사각지대를 없앤다.
//
// 침대 ROI는 Qt 관제 화면에서 그려 서버로 전송되며(protocol/video_stream.h),
// update()에 채널별 정규화 다각형(0~1)으로 넘어온다. ROI가 없으면(폴백)
// 게이트 없이 화면 전체를 관찰 대상으로 본다.
class FallDetector {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    // 낙상으로 확정되는 순간 1회 호출된다 (같은 객체당, 침대 재실 전까지 재통보 안 함).
    void setFallCallback(FallCallback cb) { on_fall_ = std::move(cb); }

    // 채널의 최신 감지 결과를 매 메타데이터 콜백(주기 ~5Hz)마다 전달.
    // bed_roi: 이 채널의 침대 ROI(정규화 0~1 다각형, 3점 이상). 비어 있으면
    // 침대 게이트 없이 화면 전체를 관찰 대상으로 본다(ROI 미설정 폴백).
    // 재실 판정 기준점은 bbox 하단 중앙("발끝") — Qt에서 ROI를 그릴 때는
    // 침대가 바닥에 차지하는 발자국+매트리스 면을 감싸게 그려야 한다.
    // 여기서는 ROI 게이팅과 bbox 캐시 갱신만 한다 — 낙상 확정은 reportPose()가 담당.
    void update(int channel, const std::vector<Detection>& detections,
                const std::vector<std::pair<float, float>>& bed_roi);

    // 현재 "관찰 대상"(침대 밖)인 사람들의 최신 bbox. main.cpp가 비디오
    // 프레임마다 이 목록으로 크롭 → MoveNet 추론 → reportPose() 순으로 호출한다.
    struct ObservedTrack {
        int object_id;
        float left, top, right, bottom;  // 정규화 0~1 (WiseAI bbox 그대로)
    };
    std::vector<ObservedTrack> observedTracks(int channel) const;

    // MoveNet 자세 판정 결과 보고. lying=true가 kLyingConfirmSec 동안
    // 연속으로 들어오면 낙상 확정 콜백을 1회 호출한다. lying=false면 리셋.
    // (channel, object_id)가 그 사이 만료되었거나 침대로 복귀했으면 무시.
    void reportPose(int channel, int object_id, bool lying);

private:
    // 사람(ObjectId) 1명의 추적 상태
    struct Track {
        bool in_bed = false;  // 직전 프레임에 침대 ROI 안이었는지 (이탈/재실 로그용)
        float left = 0, top = 0, right = 0, bottom = 0;  // 최신 bbox (크롭용)
        bool lying_active = false;  // 직전 자세 보고가 "누움"이었는지
        std::chrono::steady_clock::time_point lying_since;  // 누움이 시작된 시각
        bool fired = false;  // 이미 통보함 (재실 전까지 중복 방지)
        std::chrono::steady_clock::time_point last_seen;
    };

    // 채널 → (ObjectId → 추적 상태). 오래 안 보인 추적은 update()에서 정리.
    std::map<int, std::map<int, Track>> channels_;
    FallCallback on_fall_;
};
