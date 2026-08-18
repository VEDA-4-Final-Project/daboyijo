#ifndef CARE_TIMER_HPP
#define CARE_TIMER_HPP

#include <chrono>
#include <functional>

// 보호사 존재 신호를 받아 케어 세션(시작/종료)을 추적하고 케어시간을 계산
class CareTimer {
public:
    // 세션 종료 시 호출되는 콜백: (케어시간 초, 직전 세션과의 공백 초)
    //  gapSec 은 "이 세션이 시작될 때, 직전 유효 세션이 끝난 지 얼마나 지났나".
    //  직전 유효 세션이 없으면 kNoPrevSession(-1). 받는 쪽이 이 값을 보고
    //  잠깐 자리를 비운 것인지(→ 앞 기록에 합산) 새 케어인지(→ 새 기록) 정한다.
    using SessionCallback = std::function<void(int durationSec, double gapSec)>;

    // 직전 유효 세션이 없을 때 콜백에 넘기는 gapSec 값
    static constexpr double kNoPrevSession = -1.0;

    // 생성자. 값이 생략될 경우 쓰는 기본값 (3.0,5.0)
    CareTimer(double absentTimeoutSec = 3.0, double minSessionSec = 5.0);

    // 매 프레임 호출: 현재 보호사 감지 여부 전달
    void update(bool caregiverPresent);

    // 세션이 열린 채 종료될 때 정리
    void flush();

    // 세션 종료 시 실행할 콜백 등록 (예: care_logs 기록 — gapSec 을 보고
    // 새 행을 넣을지 직전 행에 합산할지는 받는 쪽이 정한다)
    void onSessionEnd(SessionCallback cb);

    void setChannel(int ch) { channel_ = ch; }

    // 세션 중인지 확인하게 해줌
    bool inSession() const { return inSession_; }

private:
    void endSession();
    static double sec(std::chrono::steady_clock::duration d);

    double absentTimeout_;
    double minSession_;
    bool inSession_ = false;
    int channel_ = -1;
    std::chrono::steady_clock::time_point sessionStart_;
    std::chrono::steady_clock::time_point lastSeen_;
    // 직전 "유효" 세션(minSession_ 이상이라 실제로 기록된 것)이 끝난 시각.
    // ★ 버려진 짧은 세션으로는 갱신하지 않는다 — 그건 케어로 안 치기로 했으니
    //   스쳐가는 오탐이 복귀 판정 창을 계속 뒤로 밀면 안 된다.
    std::chrono::steady_clock::time_point prevSessionEnd_;
    bool hasPrevSession_ = false;
    // 이번 세션이 열릴 때 계산해둔 공백. 콜백은 세션이 끝날 때 불리는데 그때는
    // 이미 늦어서(prevSessionEnd_ 가 갱신됨) 시작 시점에 재어둔다.
    double pendingGap_ = kNoPrevSession;
    SessionCallback onEnd_;
};

#endif // CARE_TIMER_HPP