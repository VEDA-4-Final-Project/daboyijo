#include "care_timer.hpp"
#include <iostream>

CareTimer::CareTimer(double absentTimeoutSec, double minSessionSec)
    : absentTimeout_(absentTimeoutSec), minSession_(minSessionSec) {}

// 세션 종료 시 부를 콜백 등록
void CareTimer::onSessionEnd(SessionCallback cb) {  //cb (람다) 받아옴 
    onEnd_ = std::move(cb);     // cb를 onEnd_에 저장
}


void CareTimer::update(bool caregiverPresent) {
    auto now = std::chrono::steady_clock::now();    // 매 프레임 호출해서 현재 시간 찍음 

    if (caregiverPresent) {
        lastSeen_ = now;
        if (!inSession_) {                 // 부재 -> 존재
            inSession_ = true;
            sessionStart_ = now;
            std::cout << "[세션 시작]\n";
        }
    } else if (inSession_) {
        if (sec(now - lastSeen_) >= absentTimeout_) {   // 일정 시간 미감지하면 세션 종료
            endSession();
        }
    }
}

// 열린 세션 있으면 강제 종료 
void CareTimer::flush() {
    if (inSession_) endSession();
}

// 지속시간 반환
void CareTimer::endSession() {
    //now 아니고 lastSeen_ (부재 타임아웃 3초는 세션시간에 미포함)
    int dur = static_cast<int>(sec(lastSeen_ - sessionStart_)); 
    inSession_ = false;

    // 5초 이상 지속된 경우만 세션으로 인정
    if (dur >= minSession_) {
        std::cout << "[세션 종료] 케어 시간: " << dur << "초\n";

        if (onEnd_) onEnd_(dur);           // 등록된 콜백 호출 (DB INSERT)
    } else {
        std::cout << "[무시] 너무 짧은 접촉 (" << dur << "초)\n";
    }
}

// 시간 간격 (d)을 읽을 수 있게 double로 변환
double CareTimer::sec(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}