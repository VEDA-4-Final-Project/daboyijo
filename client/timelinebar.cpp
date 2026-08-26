#include "timelinebar.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>

#include "theme.h"

namespace {
constexpr qint64 kMinute = 60LL * 1000;
constexpr qint64 kHour = 60 * kMinute;
// 창 길이 하한/상한 — 휠 확대·축소가 무한정 가지 않게 막는다.
constexpr qint64 kMinSpan = 5 * kMinute;
constexpr qint64 kMaxSpan = 48 * kHour;
constexpr int kRulerH = 18;   // 위쪽 눈금·라벨이 쓰는 높이
}  // namespace

TimelineBar::TimelineBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(58);
    setMaximumHeight(58);
    setMouseTracking(true);   // 커서 위치 시각 표시
    setCursor(Qt::PointingHandCursor);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    startMs_ = now - 12 * kHour;   // 기본 창 = NVR 보존기간(12시간)
    endMs_ = now;
}

void TimelineBar::setWindow(qint64 startMs, qint64 endMs) {
    if (endMs <= startMs) return;
    startMs_ = startMs;
    endMs_ = endMs;
    update();
}

void TimelineBar::followNow() {
    const qint64 span = endMs_ - startMs_;
    endMs_ = QDateTime::currentMSecsSinceEpoch();
    startMs_ = endMs_ - span;
    update();
}

void TimelineBar::setSpans(const QVector<Span>& spans) {
    spans_ = spans;
    update();
}

void TimelineBar::setMarkers(const QVector<Marker>& markers) {
    markers_ = markers;
    update();
}

void TimelineBar::setPlayhead(qint64 ms) {
    playheadMs_ = ms;
    update();
}

void TimelineBar::setLiveMode(bool on) {
    liveMode_ = on;
    update();
}

qreal TimelineBar::xForMs(qint64 ms) const {
    const qint64 span = endMs_ - startMs_;
    if (span <= 0) return 0;
    return qreal(ms - startMs_) / qreal(span) * width();
}

qint64 TimelineBar::msForX(qreal x) const {
    const qint64 span = endMs_ - startMs_;
    if (width() <= 0) return startMs_;
    const qreal t = qBound(0.0, x / qreal(width()), 1.0);
    return startMs_ + qint64(t * span);
}

// 눈금 간격은 "라벨이 서로 붙지 않는" 선에서 사람이 읽기 좋은 값으로 고른다.
qint64 TimelineBar::tickStepMs() const {
    const qint64 span = endMs_ - startMs_;
    if (span <= 1 * kHour) return 5 * kMinute;
    if (span <= 3 * kHour) return 15 * kMinute;
    if (span <= 6 * kHour) return 30 * kMinute;
    if (span <= 14 * kHour) return kHour;
    if (span <= 30 * kHour) return 2 * kHour;
    return 4 * kHour;
}

QRectF TimelineBar::trackRect() const {
    return QRectF(0, kRulerH + 6, width(), height() - kRulerH - 6 - 8);
}

void TimelineBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);   // 세로선은 또렷한 게 낫다

    const QColor bg(QString::fromLatin1(kCard));
    const QColor border(QString::fromLatin1(kBorder));
    const QColor sub(QString::fromLatin1(kTextSub));
    const QColor rec(QString::fromLatin1(kNormal));
    const QColor sel(QString::fromLatin1(kSelect));

    p.fillRect(rect(), bg);

    // ── ① 시각 눈금 ──────────────────────────────────────
    QFont tf = font();
    tf.setPixelSize(11);
    p.setFont(tf);
    const QFontMetrics fm(tf);
    const qint64 step = tickStepMs();
    // 첫 눈금 = 창 시작 이후 처음 오는 step 배수(로컬 자정 기준으로 떨어지게 맞춘다).
    const int tzOffsetMs = QDateTime::currentDateTime().offsetFromUtc() * 1000;
    qint64 t = ((startMs_ + tzOffsetMs) / step) * step - tzOffsetMs;
    if (t < startMs_) t += step;
    for (; t <= endMs_; t += step) {
        const qreal x = xForMs(t);
        p.setPen(QPen(border, 1));
        p.drawLine(QPointF(x, kRulerH - 4), QPointF(x, height() - 8));
        const QString label =
            QDateTime::fromMSecsSinceEpoch(t).toString(step < kHour ? "HH:mm" : "HH:mm");
        const int w = fm.horizontalAdvance(label);
        p.setPen(sub);
        p.drawText(QRectF(x - w / 2.0 - 4, 1, w + 8, kRulerH - 4),
                   Qt::AlignCenter, label);
    }

    // ── ② 녹화 구간 막대 ─────────────────────────────────
    const QRectF track = trackRect();
    QColor trackBg = border;
    trackBg.setAlpha(90);
    p.fillRect(track, trackBg);
    for (const auto& s : spans_) {
        if (s.endMs <= startMs_ || s.startMs >= endMs_) continue;
        const qreal x1 = xForMs(qMax(s.startMs, startMs_));
        const qreal x2 = xForMs(qMin(s.endMs, endMs_));
        // 세그먼트 하나가 1px도 안 되게 짧아도 존재는 보여야 한다.
        p.fillRect(QRectF(x1, track.top(), qMax(1.0, x2 - x1), track.height()), rec);
    }

    // ── ③ 이벤트 마커 ────────────────────────────────────
    for (const auto& m : markers_) {
        if (m.atMs < startMs_ || m.atMs > endMs_) continue;
        const qreal x = xForMs(m.atMs);
        p.setPen(QPen(m.color, 2));
        p.drawLine(QPointF(x, track.top() - 3), QPointF(x, track.bottom() + 3));
    }

    // ── "지금" 선 ───────────────────────────────────────
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now >= startMs_ && now <= endMs_) {
        const qreal x = xForMs(now);
        QColor nowColor = liveMode_ ? sel : sub;
        p.setPen(QPen(nowColor, liveMode_ ? 2 : 1, Qt::DashLine));
        p.drawLine(QPointF(x, kRulerH - 2), QPointF(x, height() - 6));
    }

    // ── 플레이헤드 ──────────────────────────────────────
    // 라이브 모드에선 "지금" 선이 곧 재생 위치라 따로 그리지 않는다.
    if (!liveMode_ && playheadMs_ >= startMs_ && playheadMs_ <= endMs_) {
        const qreal x = xForMs(playheadMs_);
        p.setPen(QPen(sel, 2));
        p.drawLine(QPointF(x, kRulerH - 6), QPointF(x, height() - 4));

        // 시각 말풍선 — 지금 어느 시점을 보고 있는지 숫자로도 읽히게.
        const QString label =
            QDateTime::fromMSecsSinceEpoch(playheadMs_).toString("MM-dd HH:mm:ss");
        const int w = fm.horizontalAdvance(label) + 12;
        qreal bx = qBound(0.0, x - w / 2.0, qreal(width() - w));
        QRectF bubble(bx, height() - 16, w, 15);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(sel);
        p.drawRoundedRect(bubble, 3, 3);
        p.setPen(QColor(QString::fromLatin1(kOnSelect)));
        p.drawText(bubble, Qt::AlignCenter, label);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ── 커서 위치 미리보기 ───────────────────────────────
    if (hoverX_ >= 0 && !dragging_) {
        p.setPen(QPen(sub, 1));
        p.drawLine(QPointF(hoverX_, kRulerH), QPointF(hoverX_, height() - 8));
    }

    p.setPen(QPen(border, 1));
    p.drawLine(0, 0, width(), 0);
}

void TimelineBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    const qint64 ms = msForX(e->position().x());
    setPlayhead(ms);
    emit seekRequested(ms);
}

void TimelineBar::mouseMoveEvent(QMouseEvent* e) {
    hoverX_ = e->position().x();
    if (dragging_) {
        const qint64 ms = msForX(hoverX_);
        setPlayhead(ms);
        emit seekRequested(ms);
    }
    update();
}

void TimelineBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    emit seekCommitted(msForX(e->position().x()));
}

// 휠 = 커서 시각을 고정한 채 창을 확대·축소. 관제 중 "그 몇 분"만 확대해 보는 동작.
void TimelineBar::wheelEvent(QWheelEvent* e) {
    const int steps = e->angleDelta().y();
    if (steps == 0) return;
    const qint64 span = endMs_ - startMs_;
    const qreal factor = steps > 0 ? 0.8 : 1.25;
    qint64 newSpan = qBound(kMinSpan, qint64(span * factor), kMaxSpan);
    if (newSpan == span) return;

    const qreal t = qBound(0.0, e->position().x() / qreal(qMax(1, width())), 1.0);
    const qint64 anchor = startMs_ + qint64(t * span);
    startMs_ = anchor - qint64(t * newSpan);
    endMs_ = startMs_ + newSpan;
    update();
}

void TimelineBar::leaveEvent(QEvent*) {
    hoverX_ = -1;
    update();
}
