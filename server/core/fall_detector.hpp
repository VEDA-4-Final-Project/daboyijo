#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "detection.hpp"

// WiseAI 감지 스트림만으로 낙상 후보를 판정하는 1차 룰엔진.
//
// 신호 2개만 사용한다 (실측 캡처 전 설계 — docs/wiseai-메타데이터-명세.md 참조):
//   ① 짧은 시간(kDropWindowSec) 안에 무게중심(cy)이 kDropCyThreshold 이상 하강
//   ② 그 직후 kStillSeconds 동안 그 자리에서 거의 움직이지 않음
// 둘 다 만족해야 낙상으로 통보한다 — ①만으로는 "빨리 앉기/눕기"와 구분이 안 됨.
//
// 침상 ROI 필터는 아직 없다(ROI 설정 기능 미구현). 그래서 침대 위에 빨리
// 눕는 정상 동작도 지금은 오탐 후보가 될 수 있다 — kStillSeconds가 그 오탐을
// 줄이는 유일한 방어선이므로, 캡처 데이터로 우선 튜닝할 값이다.
//
// 임계값은 전부 잠정값. server/tools/metadata_probe.py로 뜬 실측 캡처로
// 검증·조정할 것 (걷기/천천히눕기/급쓰러짐 캡처와 대조).
class FallDetector {
public:
    using FallCallback = std::function<void(int channel, const Detection& at)>;

    // 낙상으로 확정되는 순간 1회 호출된다 (같은 사건에 중복 호출 안 함).
    void setFallCallback(FallCallback cb) { on_fall_ = std::move(cb); }

    // 채널의 최신 감지 결과를 매 메타데이터 콜백(주기 ~5Hz)마다 전달.
    void update(int channel, const std::vector<Detection>& detections);

private:
    struct Sample {
        std::chrono::steady_clock::time_point t;
        float cx = 0;
        float cy = 0;
    };

    struct ChannelState {
        std::deque<Sample> history;  // 최근 kDropWindowSec 구간 표본 (급하강 판정용)
        bool watching = false;       // 급하강 감지 후 정지 관찰 중인지
        bool fired = false;          // 이번 낙상을 이미 통보했는지 (중복 통보 방지)
        std::chrono::steady_clock::time_point drop_time;
        float drop_cx = 0, drop_cy = 0;  // 급하강 시점 위치 — 정지 판정 기준점
    };

    std::map<int, ChannelState> channels_;
    FallCallback on_fall_;
};
