#ifndef ALERT_DISPLAY_HPP
#define ALERT_DISPLAY_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include "alert-event.hpp"

/*
 * 64x32 HUB75 패널에 한글 경보를 스크롤한다.
 *
 * 픽셀과 vsync 를 모두 표준 fbdev(/dev/fbN) 하나로 처리한다. 드라이버가
 * 단일 버퍼라 프레임을 오프스크린에 완성한 뒤 프레임 경계에서 한 번에
 * 복사해야 스크롤이 찢어지지 않는다.
 */
class AlertDisplay {
public:
    /* 스크롤 도중 true 를 돌려주면 즉시 중단한다 (종료 요청, 더 급한 경보) */
    using AbortFn = std::function<bool()>;

    AlertDisplay() = default;
    ~AlertDisplay();
    AlertDisplay(const AlertDisplay&) = delete;             // fd 와 mmap 을 소유한다
    AlertDisplay& operator=(const AlertDisplay&) = delete;

    bool open();                                        // fix.id 가 "hub75" 인 fb 를 찾는다
    void clear();
    bool setBrightness(int v);                          // 0~255, 드라이버 전역 설정
    /* 스크롤 - passes 번 흘리고 리턴 (1~10 으로 잘라낸다) */
    void show(const std::string& msg, severity sev, int passes,
              const AbortFn& abort = nullptr);
    /* 정지 - 한 번 그리고 바로 리턴한다. 지울 때까지 드라이버가 계속 띄운다 */
    void showStatic(const std::string& msg, severity sev);

private:
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

#endif /* ALERT_DISPLAY_HPP */
