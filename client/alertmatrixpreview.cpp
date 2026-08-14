// 64x32 HUB75 패널 미리보기 — alert-node/app/matrix/alert-display.cpp 의 이식본.
// 그리기 로직(글꼴 이진탐색·UTF-8 디코드·스크롤·테두리)은 원본과 1:1 로 맞춘다.
#include "alertmatrixpreview.h"
#include "hub75-font16.h"

#include <QPainter>
#include <QTimer>
#include <array>

namespace {

constexpr int COLS = 64;
constexpr int ROWS = 32;
constexpr int BAND = 3;        // 위아래 등급색 테두리 두께(원본 alert-display.cpp 와 동일)
constexpr int FPS_MS = 30;     // 프레임당 1px 이동 (원본 FPS_US=30000)

// UTF-8 한 글자 디코드 — *cp 에 코드포인트, 다음 위치 반환. (alert-display.cpp:32 이식)
const char* utf8Next(const char* s, uint32_t* cp)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80)           { *cp = c;                                    return s + 1; }
    if ((c & 0xE0) == 0xC0) { *cp = ((c & 0x1Fu) << 6) |
                                     ((uint8_t)s[1] & 0x3Fu);             return s + 2; }
    if ((c & 0xF0) == 0xE0) { *cp = ((c & 0x0Fu) << 12) |
                                    (((uint8_t)s[1] & 0x3Fu) << 6) |
                                     ((uint8_t)s[2] & 0x3Fu);             return s + 3; }
    *cp = '?';
    return s + 1;
}

// 코드포인트로 글리프 찾기 — 표가 cp 오름차순이라 이진탐색. (alert-display.cpp:46 이식)
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

AlertMatrixPreview::AlertMatrixPreview(QWidget* parent)
    : QWidget(parent)
{
    setText(QStringLiteral("302호 낙상 발생"));   // 폰트에 있는 대표 경보 문구
    timer_ = new QTimer(this);
    timer_->setInterval(FPS_MS);
    connect(timer_, &QTimer::timeout, this, &AlertMatrixPreview::advance);
}

void AlertMatrixPreview::setBrightness(int v255)
{
    v255 = qBound(0, v255, 255);
    if (v255 == brightness_) return;
    brightness_ = v255;
    update();               // 밝기 슬라이더 → 즉시 반영
}

void AlertMatrixPreview::setText(const QString& text)
{
    text_ = text;
    span_ = measureText(text_) + COLS;   // 오른쪽 등장 ~ 왼쪽 완전 퇴장
    offset_ = 0;
    update();
}

void AlertMatrixPreview::setAccentColor(const QColor& c)
{
    accent_ = c;
    update();
}

void AlertMatrixPreview::advance()
{
    if (span_ <= 0) return;
    offset_ = (offset_ + 1) % span_;     // 한 바퀴 돌면 처음부터 다시
    update();
}

void AlertMatrixPreview::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (timer_) timer_->start();         // 보일 때만 스크롤
}

void AlertMatrixPreview::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    if (timer_) timer_->stop();          // 다른 탭으로 가면 CPU 절약
}

int AlertMatrixPreview::measureText(const QString& s) const
{
    const QByteArray utf8 = s.toUtf8();
    int w = 0;
    for (const char* p = utf8.constData(); *p; ) {
        uint32_t cp;
        p = utf8Next(p, &cp);
        const glyph16_t* g = findGlyph(cp);
        w += g ? g->adv : 8;             // 글꼴에 없으면 빈칸 8px (원본과 동일)
    }
    return w;
}

void AlertMatrixPreview::paintEvent(QPaintEvent*)
{
    // 논리 64x32 격자에 켜짐/꺼짐을 채운 뒤, 위젯 크기에 정수배로 확대해 LED 도트로 그린다.
    std::array<bool, ROWS * COLS> lit{};   // 기본 전부 꺼짐

    // 위아래 등급색 테두리 (drawBorder 이식)
    for (int y = 0; y < BAND; ++y)
        for (int x = 0; x < COLS; ++x) {
            lit[y * COLS + x] = true;
            lit[(ROWS - 1 - y) * COLS + x] = true;
        }

    // 스크롤 문구 (drawText 이식) — 세로 중앙, x = 64 - offset 부터
    {
        const int y0 = (ROWS - FONT16_H) / 2;   // = 8
        int x = COLS - offset_;
        const QByteArray utf8 = text_.toUtf8();
        for (const char* p = utf8.constData(); *p; ) {
            uint32_t cp;
            p = utf8Next(p, &cp);
            const glyph16_t* g = findGlyph(cp);
            if (!g) { x += 8; continue; }
            if (x + 16 > 0 && x < COLS) {
                for (int row = 0; row < FONT16_H; ++row) {
                    int py = y0 + row;
                    if (py < 0 || py >= ROWS) continue;
                    uint16_t bits = g->rows[row];
                    for (int col = 0; col < 16; ++col) {
                        int px = x + col;
                        if (!(bits & (0x8000 >> col)) || px < 0 || px >= COLS) continue;
                        lit[py * COLS + px] = true;
                    }
                }
            }
            x += g->adv;
        }
    }

    // ── 그리기 ──
    const int w = width(), h = height();
    const int cell = qMax(2, qMin(w / COLS, h / ROWS));
    const int gw = cell * COLS, gh = cell * ROWS;
    const int ox = (w - gw) / 2, oy = (h - gh) / 2;
    const int gap = qMax(1, cell / 8);

    // 밝기 반영: 켜진 도트 = 등급색 * brightness/255. 꺼진 도트는 밝기와 무관한 어두운 색.
    const QColor on(accent_.red()   * brightness_ / 255,
                    accent_.green() * brightness_ / 255,
                    accent_.blue()  * brightness_ / 255);
    const QColor off(0x15, 0x18, 0x1f);
    const QColor bezel(0x0a, 0x0c, 0x11);

    QPainter pt(this);
    pt.setRenderHint(QPainter::Antialiasing, false);
    pt.fillRect(rect(), bezel);
    pt.setPen(Qt::NoPen);

    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x) {
            pt.setBrush(lit[y * COLS + x] ? on : off);
            pt.drawRect(ox + x * cell + gap, oy + y * cell + gap,
                        cell - 2 * gap, cell - 2 * gap);
        }
}
