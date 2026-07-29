#ifndef CARE_TIMER_HPP
#define CARE_TIMER_HPP

#include <chrono>
#include <functional>

// 보호사 존재 신호를 받아 케어 세션(시작/종료)을 추적하고 케어시간을 계산
class CareTimer {
public:
    // 세션 종료 시 호출되는 콜백: (케어시간 초)
    using SessionCallback = std::function<void(int durationSec)>;

    // 생성자. 값이 생략될 경우 쓰는 기본값 (3.0,5.0)
    CareTimer(double absentTimeoutSec = 3.0, double minSessionSec = 5.0);

    // 매 프레임 호출: 현재 보호사 감지 여부 전달
    void update(bool caregiverPresent);

    // 세션이 열린 채 종료될 때 정리
    void flush();

    // 세션 종료 시 실행할 콜백 등록 (예: care_log INSERT)
    void onSessionEnd(SessionCallback cb);

    // 세션 중인지 확인하게 해줌
    bool inSession() const { return inSession_; }

private:
    void endSession();
    static double sec(std::chrono::steady_clock::duration d);

    double absentTimeout_;
    double minSession_;
    bool inSession_ = false;
    std::chrono::steady_clock::time_point sessionStart_;
    std::chrono::steady_clock::time_point lastSeen_;
    SessionCallback onEnd_;
};

#endif // CARE_TIMER_HPP