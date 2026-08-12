// 패널 알림 데모 — 이벤트를 난수로 지어내 AlertDisplay 로 흘림
//
// 하드웨어 연동 없이 패널과 글꼴 동작만 확인하는 용도
// 실제 알림 노드는 이 자리에 MQTT 소스를 꽂음 (app/main.cpp)
//
// 빌드: make          실행: sudo ./alert-demo      종료: Ctrl-C
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "alert-display.hpp"

namespace {

volatile sig_atomic_t running = 1;

void onSignal(int) { running = 0; }

const int rooms[] = { 301, 302, 303, 305, 308, 311, 315 };

// alert_source_fn 구현 — 이벤트 하나를 지어내 ev 를 채움
// MQTT 연동은 같은 시그니처의 소스를 만들어 갈아끼우면 됨
int demoNextEvent(alert_event* ev)
{
    int room = rooms[rand() % (sizeof(rooms) / sizeof(rooms[0]))];
    size_t n = sizeof(ev->msg);

    switch (rand() % 5) {
    case 0:
        ev->sev = SEV_CRIT;
        snprintf(ev->msg, n, "긴급 %d호 낙상 발생 즉시 확인 요망", room);
        break;
    case 1:
        ev->sev = SEV_CRIT;
        snprintf(ev->msg, n, "긴급 %d호 침상 이탈 감지", room);
        break;
    case 2:
        ev->sev = SEV_WARN;
        snprintf(ev->msg, n, "주의 %d호 고열 체온 %d.%d도",
                 room, 38 + rand() % 2, rand() % 10);
        break;
    case 3:
        ev->sev = SEV_WARN;
        snprintf(ev->msg, n, "주의 %d호 심박 이상 분당 %d회",
                 room, 120 + rand() % 40);
        break;
    default:
        ev->sev = SEV_WARN;
        snprintf(ev->msg, n, "주의 %d호 산소포화도 %d%%", room, 85 + rand() % 5);
        break;
    }

    // 긴급은 한 번 더 반복해 확실히 알림
    ev->passes = (ev->sev == SEV_CRIT) ? 3 : 2;
    return 1;
}

} // namespace

int main()
{
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    srand((unsigned)time(nullptr));

    AlertDisplay display;
    if (!display.open()) {
        fprintf(stderr, "hub75 프레임버퍼 못 찾음 (드라이버 로드? sudo?)\n");
        return 1;
    }

    printf("요양원 알림 데모 시작 - Ctrl-C 로 종료\n");

    alert_source_fn nextEvent = demoNextEvent;
    auto aborted = [](int) { return !running; };

    while (running) {
        alert_event ev;

        display.showStatic("감시 중", SEV_INFO);   // 평상시 — 그리고 바로 리턴
        if (!nextEvent(&ev))
            continue;

        printf("[이벤트] %s\n", ev.msg);
        display.show(ev.msg, ev.sev, ev.passes, aborted);
    }

    display.clear();
    printf("\n종료했다\n");
    return 0;
}
