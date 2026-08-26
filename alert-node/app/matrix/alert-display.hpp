#ifndef ALERT_DISPLAY_HPP
#define ALERT_DISPLAY_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include "alert-event.hpp"

// 64x32 HUB75 패널에 한글 경보 스크롤
//
// 픽셀과 vsync 를 표준 fbdev(/dev/fbN) 하나로 처리
// 드라이버가 단일 버퍼라 오프스크린에 완성한 뒤 프레임 경계에서 한 번에
// 복사해야 스크롤이 안 찢어짐
class AlertDisplay {
public:
    // 스크롤 중 프레임마다 호출 — true 면 즉시 중단
    // passesDone 은 지금까지 끝낸 바퀴 수, 언제 끊을지는 호출자가 결정
    using AbortFn = std::function<bool(int passesDone)>;

    AlertDisplay() = default;
    ~AlertDisplay();
    AlertDisplay(const AlertDisplay&) = delete;         // fd 와 mmap 을 소유
    AlertDisplay& operator=(const AlertDisplay&) = delete;

    bool open();                                        // fix.id 가 "hub75" 인 fb 탐색
    void clear();
    // 종료 경로 전용 — vsync 를 기다리지 않고 바로 지운다.
    // 패널 커널 스레드가 멎어 있으면 FBIO_WAITFORVSYNC 가 영영 안 돌아와
    // 정리가 통째로 매달린다. 프레임 경계를 안 맞춰 한 프레임이 찢어질 수
    // 있지만 어차피 검은 화면이라 티가 나지 않는다.
    void clearNoWait();
    bool setBrightness(int v);                          // 0~255, 드라이버 전역 설정
    // 위아래 테두리를 등급색으로 깜빡임 (스크롤 중 깜빡임의 2배 속도)
    // 같은 문구가 연달아 뜰 때 새 경보라는 신호
    // abort 는 프레임마다 호출 — true 면 즉시 중단(종료 신호에 바로 응답하려고 받는다)
    void blinkCue(severity sev, int times = 2, const AbortFn& abort = nullptr);
    // 스크롤 — passes 번 흘리고 리턴 (1~10 으로 클램프)
    void show(const std::string& msg, severity sev, int passes,
              const AbortFn& abort = nullptr);
    // 스크롤 — abort 가 끊을 때까지. 안 끊으면 안 돌아오니 abort 없이 부르면 안 됨
    void showUntilAborted(const std::string& msg, severity sev, const AbortFn& abort);
    // 정지 표시 — 한 번 그리고 리턴, 지울 때까지 드라이버가 계속 띄움
    void showStatic(const std::string& msg, severity sev);

private:
    // show 와 showUntilAborted 의 알맹이 — passes < 0 이면 무한
    // 무한을 passes 값으로 공개 안 하는 건 그게 MQTT 로 들어오는 값이라서
    void scroll(const std::string& msg, severity sev, int passes, const AbortFn& abort);
    void drawText(int x, int y, const std::string& s, const uint8_t rgb[3]);
    void drawBorder(const uint8_t rgb[3], int scale);
    int  measureText(const std::string& s) const;
    void flush();                                       // vsync 대기 후 한 번에 복사

    int      fbfd_ = -1;
    uint8_t* fb_ = nullptr;                             // mmap 된 프레임버퍼
    size_t   mapSize_ = 0;
    std::vector<uint8_t> scratch_;                      // 오프스크린 합성용
    int cols_ = 0, rows_ = 0, stride_ = 0;
};

#endif // ALERT_DISPLAY_HPP
