#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "detection.hpp"

// WiseAI 감지 스트림만으로 낙상 후보를 판정하는 1차 룰엔진.
//
// 사람(Human) 객체를 ObjectId별로 따로 추적한다 — 화면에 여러 명이 있어도
// 각자 자기 이동 이력으로만 판정한다. (채널당 대표 1명만 보면 사람 간에
// 대표가 바뀔 때 가짜 '급하강'이 생겨 오탐이 남 — 실측으로 확인된 결함)
//
// 판정 신호 2개 (docs/wiseai-메타데이터-명세.md 참조):
//   ① 같은 객체의 무게중심(cy)이 짧은 시간(kDropWindowSec) 안에
//      kDropCyThreshold 이상 하강
//   ② 그 직후 kStillSeconds 동안 그 자리에서 거의 움직이지 않음
// 둘 다 만족해야 통보한다 — ①만으로는 "빨리 앉기/눕기"와 구분이 안 됨.
//
// 참고: WiseAI는 추적이 몇 초 끊기면 같은 사람에게 새 ObjectId를 준다.
// 새 ID는 빈 이력에서 시작하므로 사람 간 좌표 차이로 인한 오탐은 없다.
//
// 침상 ROI 필터는 아직 없다(ROI 설정 기능 미구현). 침대 위에 빨리 눕는
// 정상 동작이 오탐 후보가 될 수 있고, kStillSeconds가 유일한 방어선이다.
// 임계값은 전부 잠정값 — 실측 캡처(metadata_probe.py)로 튜닝할 것.
class FallDetector {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    // 낙상으로 확정되는 순간 1회 호출된다 (같은 객체당 1회).
    void setFallCallback(FallCallback cb) { on_fall_ = std::move(cb); }

    // 채널의 최신 감지 결과를 매 메타데이터 콜백(주기 ~5Hz)마다 전달.
    void update(int channel, const std::vector<Detection>& detections);

private:
    struct Sample {
        std::chrono::steady_clock::time_point t;
        float cx = 0;
        float cy = 0;
    };

    // 사람(ObjectId) 1명의 추적 상태
    struct Track {
        std::deque<Sample> history;  // 최근 kDropWindowSec 구간 표본
        bool watching = false;       // 급하강 감지 후 정지 관찰 중
        bool fired = false;          // 이미 통보함 (중복 방지)
        std::chrono::steady_clock::time_point drop_time;
        float drop_cx = 0, drop_cy = 0;  // 급하강 시점 위치 — 정지 판정 기준점
        std::chrono::steady_clock::time_point last_seen;
    };

    // 채널 → (ObjectId → 추적 상태). 오래 안 보인 추적은 update()에서 정리.
    std::map<int, std::map<int, Track>> channels_;
    FallCallback on_fall_;
};
