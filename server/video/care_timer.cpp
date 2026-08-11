#include "care_timer.hpp"
#include <cstdio>

CareTimer::CareTimer(double absentTimeoutSec, double minSessionSec)
    : absentTimeout_(absentTimeoutSec), minSession_(minSessionSec) {}

void CareTimer::onSessionEnd(SessionCallback cb) {
    onEnd_ = std::move(cb);
}

void CareTimer::update(bool caregiverPresent) {
    auto now = std::chrono::steady_clock::now();

    if (caregiverPresent) {
        lastSeen_ = now;
        if (!inSession_) {                 // 부재 -> 존재
            inSession_ = true;
            sessionStart_ = now;
            // 직전 유효 세션이 끝난 뒤 얼마나 비었는지 지금 재둔다.
            pendingGap_ = hasPrevSession_ ? sec(now - prevSessionEnd_)
                                          : kNoPrevSession;
            // ★ cout에서 stderr로 변경 — 파이프 버퍼링 때문에 세션 로그가
            //   caregiver 로그 뒤에 묻혀 안 보이던 문제 해결
            std::fprintf(stderr, "[ch%d] [세션 시작]\n",channel_ +1);
        }
    } else if (inSession_) {
        if (sec(now - lastSeen_) >= absentTimeout_) {   // 일정 시간 미감지
            endSession();
        }
    }
}

void CareTimer::flush() {
    if (inSession_) endSession();
}

void CareTimer::endSession() {
    int dur = static_cast<int>(sec(lastSeen_ - sessionStart_));
    inSession_ = false;
    if (dur >= minSession_) {
        std::fprintf(stderr, "[ch%d] [세션 종료] 케어 시간: %d초\n", channel_ +1, dur);
        if (onEnd_) onEnd_(dur, pendingGap_);  // 등록된 콜백 호출 (예: DB 기록)
        // 유효 세션만 복귀 판정의 기준점이 된다.
        prevSessionEnd_ = lastSeen_;
        hasPrevSession_ = true;
    } else {
        // 버리는 세션은 prevSessionEnd_ 를 건드리지 않는다 — 케어로 안 친 접촉이
        // 복귀 판정 창을 연장해버리면 한참 전에 끝난 케어에 엉뚱하게 합산된다.
        std::fprintf(stderr, "[ch%d] [무시] 너무 짧은 접촉 (%d초)\n", channel_ + 1, dur);
    }
}

double CareTimer::sec(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}