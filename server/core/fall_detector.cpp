#include "fall_detector.hpp"

#include <cmath>

namespace {

// ── 잠정 기본값 (실측 캡처로 튜닝 전) ────────────────────────
// 걷기/천천히눕기/급쓰러짐 캡처 확보 후 이 파일만 고치면 된다.
constexpr double kDropWindowSec = 0.6;      // 이 시간 안의 하강만 "급함"으로 침
constexpr float kDropCyThreshold = 0.15f;   // 정규화 좌표 기준 무게중심 하강폭
constexpr double kStillSeconds = 3.0;       // 급하강 후 이만큼 안 움직이면 낙상 확정
constexpr float kStillMoveThreshold = 0.04f;  // 이 이하 이동은 "정지"로 간주
constexpr double kLostGiveUpSec = kStillSeconds * 2;  // 관찰 중 추적 소실 시 포기 시한

}  // namespace

void FallDetector::update(int channel, const std::vector<Detection>& detections) {
    auto& st = channels_[channel];
    auto now = std::chrono::steady_clock::now();

    // 이번 프레임의 대표 사람 1명만 사용 (Human 중 likelihood 최고).
    // bbox 넓이 0인 요약 프레임(객체 소실 시 옷 색상 등을 담아 오는 프레임)은 제외.
    const Detection* person = nullptr;
    for (const auto& d : detections) {
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;
        if (!person || d.likelihood > person->likelihood) person = &d;
    }

    if (!person) {
        // 사람이 안 보임. 관찰(watching) 중이 아니면 그냥 대기.
        // 관찰 중이었다면 — 급하강 직후 추적이 끊긴 것도 그 자체로 의심 신호일 수 있어
        // 바로 취소하지 않고 유지하되, 너무 오래 안 보이면(다른 문제로 보고) 포기한다.
        if (st.watching &&
            std::chrono::duration<double>(now - st.drop_time).count() > kLostGiveUpSec) {
            st.watching = false;
        }
        return;
    }

    st.history.push_back({now, person->cx, person->cy});
    while (!st.history.empty() &&
           std::chrono::duration<double>(now - st.history.front().t).count() >
               kDropWindowSec) {
        st.history.pop_front();
    }

    if (!st.watching) {
        // 최근 창 안에서 가장 낮았던(화면 위쪽=cy 작음) 값 대비 현재 하강폭 확인
        float min_cy = st.history.front().cy;
        for (const auto& s : st.history) min_cy = std::min(min_cy, s.cy);
        if (person->cy - min_cy >= kDropCyThreshold) {
            st.watching = true;
            st.fired = false;
            st.drop_time = now;
            st.drop_cx = person->cx;
            st.drop_cy = person->cy;
        }
        return;
    }

    // 관찰 중: 급하강 시점 위치에서 계속 안 움직이는지 확인.
    float moved = std::hypot(person->cx - st.drop_cx, person->cy - st.drop_cy);
    if (moved > kStillMoveThreshold) {
        st.watching = false;  // 다시 움직임 = 낙상 아님(빨리 앉았다가 자세 고침 등)
        return;
    }

    double elapsed = std::chrono::duration<double>(now - st.drop_time).count();
    if (elapsed >= kStillSeconds && !st.fired) {
        st.fired = true;
        if (on_fall_) on_fall_(channel, *person);
    }
}
