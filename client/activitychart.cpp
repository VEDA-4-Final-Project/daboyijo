#include "activitychart.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRectF>

#include "theme.h"

namespace {
// 그리는 영역 여백 — 좌측에 걸음 눈금이 들어간다.
constexpr int kPadL = 44, kPadR = 14, kPadT = 14, kPadB = 26;
constexpr int kHours = 24;

// 걸음 축 최댓값을 보기 좋은 수로 올림 (막대 꼭대기가 눈금과 맞도록).
int niceCeil(int v) {
    if (v <= 0) return 100;
    int step = 100;
    while (v > step * 5) step *= 2;
    return ((v + step - 1) / step) * step;
}
}  // namespace

ActivityChart::ActivityChart(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(190);
}

void ActivityChart::setData(const QVector<int>& stepsPerHour) {
    // assign() 대신 재대입 — assign 은 Qt 6.6 부터라 6.5 에서 빌드가 깨진다.
    steps_ = QVector<int>(kHours, 0);
    for (int i = 0; i < kHours && i < stepsPerHour.size(); ++i) steps_[i] = stepsPerHour[i];

    hasData_ = false;
    for (int i = 0; i < kHours; ++i)
        if (steps_[i] > 0) { hasData_ = true; break; }
    update();
}

void ActivityChart::clear() {
    steps_.clear();
    hasData_ = false;
    update();
}

void ActivityChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 팔레트 전역은 const char* 라 QString 을 거쳐 QColor 로 만든다
    // (이 저장소의 다른 위젯들과 같은 방식 — videoview.cpp 참고).
    const QColor accent(QString::fromLatin1(kAccent));
    const QColor sub(QString::fromLatin1(kTextSub));
    const QColor border(QString::fromLatin1(kBorder));

    // 값이 하나도 없으면 안내만 — 빈 축만 덩그러니 그리면 고장난 것처럼 보인다.
    if (!hasData_ || steps_.size() < kHours) {
        p.setPen(sub);
        QFont f = font();
        f.setPointSize(10);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("이 날짜의 활동 기록이 없습니다"));
        return;
    }

    const QRectF plot(kPadL, kPadT,
                      qMax(10, width() - kPadL - kPadR),
                      qMax(10, height() - kPadT - kPadB));

    int stepMax = 0;
    for (int i = 0; i < kHours; ++i) stepMax = qMax(stepMax, steps_[i]);
    const int axisMax = niceCeil(stepMax);

    QFont small = font();
    small.setPointSize(8);
    p.setFont(small);
    const QFontMetrics fm(small);

    // ── 가로 눈금선 + 좌측 걸음 눈금 ──
    p.setPen(QPen(border, 1, Qt::DotLine));
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p.setPen(sub);
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        const QString t = QString::number(axisMax * g / 2);
        p.drawText(QPointF(plot.left() - 6 - fm.horizontalAdvance(t), y + fm.ascent() / 2.0 - 1), t);
    }

    // ── 걸음 막대 ──
    // 막대 사이를 조금 띄워 24칸이 각각 한 시간이라는 게 보이게 한다.
    const double slot = plot.width() / kHours;
    const double barW = qMax(2.0, slot * 0.62);
    for (int h = 0; h < kHours; ++h) {
        if (steps_[h] <= 0) continue;
        const double bh = plot.height() * steps_[h] / double(axisMax);
        const QRectF bar(plot.left() + slot * h + (slot - barW) / 2.0,
                         plot.bottom() - bh, barW, bh);
        // 가장 활발했던 시간만 진하게 — 하루의 정점이 한눈에 보이도록.
        QColor c = accent;
        c.setAlpha(steps_[h] == stepMax ? 255 : 150);
        p.fillRect(bar, c);
    }

    // ── 아래 시간 눈금 (3시간 간격) ──
    p.setPen(sub);
    for (int h = 0; h < kHours; h += 3) {
        const double x = plot.left() + slot * h + slot / 2.0;
        const QString t = QString::number(h);
        p.drawText(QPointF(x - fm.horizontalAdvance(t) / 2.0, plot.bottom() + fm.ascent() + 5), t);
    }
}
