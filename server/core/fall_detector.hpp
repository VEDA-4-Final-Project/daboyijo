#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "detection.hpp"

// WiseAI 감지 스트림만으로 낙상 후보를 판정하는 1차 룰엔진.
//
// 사람(Human) 객체를 ObjectId별로 따로 추적한다 — 화면에 여러 명이 있어도
// 각자 자기 이동 이력으로만 판정한다. (채널당 대표 1명만 보면 사람 간에
// 대표가 바뀔 때 가짜 '급하강'이 생겨 오탐이 남 — 실측으로 확인된 결함)
//
// 판정 신호 (docs/wiseai-메타데이터-명세.md 참조):
//   ⓪ (게이트) 침대 ROI 밖일 때만 판정한다 — 침대 안 취침·뒤척임은 전부 무시.
//      침대에서 나오는 순간(이탈) 관찰모드에 진입하고 기준점을 리셋한다
//      (침대에서 내려오는 동작 자체가 급하강으로 오탐되는 것을 막기 위함).
//   ① 머리(Head) cy가 짧은 시간(kDropWindowSec) 안에 kHeadDropThreshold 이상 급강하
//      (서 있음=화면 위, 쓰러짐=바닥. 무게중심보다 낙하가 뚜렷해 이걸 트리거로 씀)
//   ② 그 직후 kStillSeconds 동안 그 자리에서 거의 움직이지 않음
//   ③ (보조) 그 사람의 머리가 화면 바닥 근처(cy 큼)면 정지 대기 없이 즉시
// 기본: ⓪ 관찰모드 안에서 ① + ③ 이면 즉시 통보(B모드), ③이 없으면 ① + ② 로 통보.
// ①만으로는 "빨리 앉기/눕기"와 구분이 안 되므로 항상 ② 또는 ③을 함께 본다.
//
// 신호 모드는 fall_detector.cpp 상단 kHeadInstantTrigger로 토글:
//   true(B, 기본) — 머리 바닥 근처면 정지 안 기다리고 즉시 알람 (빠름)
//   false(A)      — 머리 바닥 근처면 정지 대기시간만 단축 (보수적)
//
// 참고: WiseAI는 추적이 몇 초 끊기면 같은 사람에게 새 ObjectId를 준다.
// 새 ID는 빈 이력에서 시작하므로 사람 간 좌표 차이로 인한 오탐은 없다.
// 또한 이 카메라의 Head 감지는 불안정(자주 누락)하므로, 머리는 어디까지나
// 보조 신호다 — 머리가 없어도 몸통 신호(①+②)만으로 판정된다.
//
// 침대 ROI는 Qt 관제 화면에서 그려 서버로 전송되며(protocol/video_stream.h),
// update()에 채널별 정규화 다각형(0~1)으로 넘어온다. ROI가 없으면(폴백)
// 게이트 없이 화면 전체에서 판정한다. 임계값은 전부 잠정값(캡처로 튜닝).
class FallDetector {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    // 낙상으로 확정되는 순간 1회 호출된다 (같은 객체당 1회).
    void setFallCallback(FallCallback cb) { on_fall_ = std::move(cb); }

    // 채널의 최신 감지 결과를 매 메타데이터 콜백(주기 ~5Hz)마다 전달.
    // bed_roi: 이 채널의 침대 ROI(정규화 0~1 다각형, 3점 이상). 비어 있으면
    // 침대 게이트 없이 화면 전체에서 판정한다(ROI 미설정 폴백).
    void update(int channel, const std::vector<Detection>& detections,
                const std::vector<std::pair<float, float>>& bed_roi);

private:
    struct Sample {
        std::chrono::steady_clock::time_point t;
        float cx = 0;
        float cy = 0;
    };

    // 사람(ObjectId) 1명의 추적 상태
    struct Track {
        // 머리 cy 이력(감지된 프레임만). 서 있으면 작고 쓰러지면 커진다 →
        // 최근 최고점(min) 대비 현재값의 급증 = 낙하. cx는 안 씀(0).
        std::deque<Sample> head_history;
        bool in_bed = false;         // 직전 프레임에 침대 ROI 안이었는지 (이탈 감지용)
        bool watching = false;       // 급하강 감지 후 정지 관찰 중
        bool fired = false;          // 이미 통보함 (중복 방지)
        std::chrono::steady_clock::time_point drop_time;
        float drop_cx = 0, drop_cy = 0;  // 급하강 시점 위치 — 정지 판정 기준점
        float last_cx = 0.0f, last_cy = 0.0f; // 바로 직전 프레임의 위치 — 실시간 순간 변위(속도) 판정용
        std::chrono::steady_clock::time_point last_seen;
    };

    // 채널 → (ObjectId → 추적 상태). 오래 안 보인 추적은 update()에서 정리.
    std::map<int, std::map<int, Track>> channels_;
    FallCallback on_fall_;
};
