// 경보 표시기
//
// 그리기는 전부 scratch_ 에 하고 flush() 가 프레임 경계에서 한 번에 복사
// fb_ 에 직접 그리면 스캔이 그리는 도중을 앞질러 스크롤이 찢어짐 (단일 버퍼)
#include "alert-display.hpp"
#include "hub75-font16.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

namespace {

constexpr int FPS_US = 30000;    // 프레임 간격 — 프레임당 1px 이동
constexpr int BAND   = 3;        // 위아래 테두리 두께
constexpr int BLINK_FRAMES = 15; // 긴급 깜빡임 한 상태의 프레임 수 (약 1Hz)

// 등급별 대표색
const uint8_t* sevColor(severity s)
{
    static const uint8_t info[3] = {   0, 200,  60 };
    static const uint8_t warn[3] = { 255, 140,   0 };
    static const uint8_t crit[3] = { 255,  30,  30 };
    return s == SEV_INFO ? info : s == SEV_WARN ? warn : crit;
}

// UTF-8 한 글자 디코드 — *cp 에 코드포인트, 다음 위치 반환 (한글은 3바이트)
const char* utf8Next(const char* s, uint32_t* cp)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80)           { *cp = c;                                        return s + 1; }
    if ((c & 0xE0) == 0xC0) { *cp = ((c & 0x1Fu) << 6) |
                                     ((uint8_t)s[1] & 0x3Fu);                 return s + 2; }
    if ((c & 0xF0) == 0xE0) { *cp = ((c & 0x0Fu) << 12) |
                                    (((uint8_t)s[1] & 0x3Fu) << 6) |
                                     ((uint8_t)s[2] & 0x3Fu);                 return s + 3; }
    *cp = '?';
    return s + 1;
}

// 코드포인트로 글리프 찾기 — 표가 cp 오름차순이라 이진탐색
const glyph16_t* findGlyph(uint32_t cp)
{
    int lo = 0, hi = (int)FONT16_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font16[mid].cp == cp) return &font16[mid];
        if (font16[mid].cp < cp) lo = mid + 1;
        else                     hi = mid - 1;
    }
    return nullptr;
}

} // namespace

AlertDisplay::~AlertDisplay()
{
    if (fb_) munmap(fb_, mapSize_);
    if (fbfd_ >= 0) ::close(fbfd_);
}

bool AlertDisplay::open()
{
    fb_var_screeninfo var;
    fb_fix_screeninfo fix;

    // fb 번호는 HDMI 유무에 따라 바뀌므로 fix.id 로 탐색
    for (int i = 0; i < 32 && fbfd_ < 0; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fb%d", i);
        int fd = ::open(path, O_RDWR);
        if (fd < 0) continue;
        if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 &&
            strcmp(fix.id, "hub75") == 0 &&
            ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            var.bits_per_pixel == 24)
            fbfd_ = fd;
        else
            ::close(fd);
    }
    if (fbfd_ < 0) return false;

    cols_ = var.xres; rows_ = var.yres; stride_ = fix.line_length;
    mapSize_ = fix.smem_len;
    fb_ = (uint8_t*)mmap(nullptr, mapSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd_, 0);
    if (fb_ == MAP_FAILED) {
        fb_ = nullptr;
        ::close(fbfd_);
        fbfd_ = -1;
        return false;
    }
    scratch_.assign((size_t)rows_ * stride_, 0);
    return true;
}

void AlertDisplay::flush()
{
    ioctl(fbfd_, FBIO_WAITFORVSYNC, 0);     // 프레임 경계까지 잔다
    memcpy(fb_, scratch_.data(), scratch_.size());
}

void AlertDisplay::clear()
{
    if (!fb_) return;
    memset(scratch_.data(), 0, scratch_.size());
    flush();
}

// 밝기는 패널 전역 설정이라 프레임버퍼가 아닌 드라이버 파라미터로 감
// ioctl 로도 되지만 /dev/hub75 를 따로 열어야 해서 sysfs 사용 (fbdev 하나로 자족)
bool AlertDisplay::setBrightness(int v)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;

    FILE* f = fopen("/sys/module/hub75/parameters/brightness", "w");
    if (!f) return false;
    bool ok = fprintf(f, "%d", v) > 0;
    fclose(f);
    return ok;
}

// 위아래 테두리를 등급색으로 빠르게 깜빡임
//
// 스크롤 중 깜빡임(BLINK_FRAMES)의 절반 주기 — 같은 리듬이면 새 경보인지
// 진행 중인 경보인지 구분이 안 됨
// 화면 전체는 눈이 부시고 껐다 켜면 잘 안 보여서 테두리만 사용
void AlertDisplay::blinkCue(severity sev, int times)
{
    if (!fb_) return;

    const uint8_t* col = sevColor(sev);
    const int half = BLINK_FRAMES / 2;          // 두 배 속도

    // show() 와 같은 프레임 간격을 써야 BLINK_FRAMES 의 절반이 실제 2배 속도가 됨
    // usleep 을 빼면 vsync 주기로만 흘러 훨씬 빨라짐
    for (int t = 0; t < times; t++) {
        for (int f = 0; f < half; f++) {
            memset(scratch_.data(), 0, scratch_.size());
            drawBorder(col, 255);
            flush();
            usleep(FPS_US);
        }
        for (int f = 0; f < half; f++) {
            memset(scratch_.data(), 0, scratch_.size());
            flush();
            usleep(FPS_US);
        }
    }
}

int AlertDisplay::measureText(const std::string& s) const
{
    int w = 0;
    for (const char* p = s.c_str(); *p; ) {
        uint32_t cp;
        p = utf8Next(p, &cp);
        const glyph16_t* g = findGlyph(cp);
        w += g ? g->adv : 8;                // 글꼴에 없으면 빈칸 8px
    }
    return w;
}

// 문구를 (x, y) 부터 한 색으로 그림 — 화면 밖은 잘라냄
void AlertDisplay::drawText(int x, int y, const std::string& s, const uint8_t rgb[3])
{
    for (const char* p = s.c_str(); *p; ) {
        uint32_t cp;
        p = utf8Next(p, &cp);
        const glyph16_t* g = findGlyph(cp);
        if (!g) { x += 8; continue; }

        if (x + 16 > 0 && x < cols_)
            for (int row = 0; row < FONT16_H; row++) {
                int py = y + row;
                if (py < 0 || py >= rows_) continue;
                uint16_t bits = g->rows[row];
                for (int col = 0; col < 16; col++) {
                    int px = x + col;
                    if (!(bits & (0x8000 >> col)) || px < 0 || px >= cols_) continue;
                    memcpy(&scratch_[py * stride_ + px * 3], rgb, 3);
                }
            }
        x += g->adv;
    }
}

// 위아래 등급색 띠 — scale 은 0=꺼짐, 255=최대 (깜빡임용)
void AlertDisplay::drawBorder(const uint8_t rgb[3], int scale)
{
    const uint8_t c[3] = { (uint8_t)(rgb[0] * scale / 255),
                           (uint8_t)(rgb[1] * scale / 255),
                           (uint8_t)(rgb[2] * scale / 255) };
    for (int y = 0; y < BAND; y++)
        for (int x = 0; x < cols_; x++) {
            memcpy(&scratch_[y * stride_ + x * 3], c, 3);
            memcpy(&scratch_[(rows_ - 1 - y) * stride_ + x * 3], c, 3);
        }
}

// 화면보다 긴 문구는 양옆이 잘림 — 짧게 넘기는 건 호출자 몫
void AlertDisplay::showStatic(const std::string& msg, severity sev)
{
    if (!fb_) return;

    const uint8_t* col = sevColor(sev);
    memset(scratch_.data(), 0, scratch_.size());
    drawBorder(col, 255);
    drawText((cols_ - measureText(msg)) / 2, (rows_ - FONT16_H) / 2, msg, col);
    flush();
}

void AlertDisplay::show(const std::string& msg, severity sev, int passes,
                        const AbortFn& abort)
{
    if (!fb_) return;

    // MQTT 로 들어오는 값이라 범위를 안 믿음
    // 0 이면 조용히 아무것도 안 뜨고, 큰 값이면 몇 분씩 붙잡혀 둘 다 디버깅이 나쁨
    if (passes < 1)  passes = 1;
    if (passes > 10) passes = 10;

    const uint8_t* col = sevColor(sev);
    const int y    = (rows_ - FONT16_H) / 2;        // 세로 중앙
    const int span = measureText(msg) + cols_;      // 오른쪽 등장 ~ 왼쪽 퇴장
    long frame = 0;

    for (int pass = 0; pass < passes; pass++)
        for (int off = 0; off <= span; off++, frame++) {
            if (abort && abort(pass)) return;

            // 긴급은 약 1Hz 로 테두리 깜빡임
            int scale = (sev == SEV_CRIT && (frame / BLINK_FRAMES) % 2) ? 40 : 255;

            memset(scratch_.data(), 0, scratch_.size());
            drawBorder(col, scale);
            drawText(cols_ - off, y, msg, col);
            flush();
            usleep(FPS_US);
        }
}
