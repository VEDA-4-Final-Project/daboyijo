#include "qmlitems.h"

#include <QCursor>
#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "frameprovider.h"
#include "theme.h"
#include "uiicons.h"

namespace {
// 침대 ROI 색 — videoview.cpp kZoneColors와 같은 값(번호별 고정 배정)
const QColor kZoneColors[] = {
    QColor(47, 158, 143), QColor(93, 143, 214), QColor(214, 149, 61), QColor(178, 122, 201),
    QColor(96, 176, 96),  QColor(214, 106, 122), QColor(122, 176, 194), QColor(184, 160, 96),
};
// 위젯 앱 QSS의 QWidget { font-size: 13px } 와 같은 기준 글꼴
QFont baseFont(int px = 13)
{
    QFont f = QGuiApplication::font();
    f.setPixelSize(px);
    return f;
}
}  // namespace

// ═══════════════════════════════════════════════════════════
//  VideoItem — videoview.cpp paintEvent()의 관제 표시 부분
// ═══════════════════════════════════════════════════════════
FrameProvider* VideoItem::source_ = nullptr;

VideoItem::VideoItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    alertTimer_.setInterval(450);   // 위젯과 같은 점멸 주기
    connect(&alertTimer_, &QTimer::timeout, this, [this] {
        alertBlink_ = !alertBlink_;
        update();
    });
}

void VideoItem::reload()
{
    if (source_) frame_ = source_->frame(channel_);
    update();
    emit changed();
}

// ── ROI 편집 (videoview.cpp 의 그리기 규칙 그대로) ──
namespace { constexpr int kMaxPoints = 32; constexpr double kDupEps = 0.006; constexpr int kMaxRoiZonesQml = 8; }

void VideoItem::setDrawMode(bool on)
{
    if (on == drawMode_) return;
    drawMode_ = on;
    draft_.clear();
    hasHover_ = false;
    setCursor(QCursor(on ? Qt::CrossCursor : Qt::ArrowCursor));
    update();
    emit drawModeChanged();
}

int VideoItem::nextFreeZoneId() const
{
    for (int id = 0; id < kMaxRoiZonesQml; ++id) {
        bool used = false;
        for (const QVariant& zv : zones_)
            if (zv.toMap().value(QStringLiteral("id")).toInt() == id) { used = true; break; }
        if (!used) return id;
    }
    return -1;
}

QPointF VideoItem::toNorm(const QPointF& w) const
{
    const QRectF r = displayRect();
    return QPointF(qBound(0.0, (w.x() - r.left()) / r.width(), 1.0),
                   qBound(0.0, (w.y() - r.top()) / r.height(), 1.0));
}

int VideoItem::zoneAt(const QPointF& n) const
{
    for (int i = zones_.size() - 1; i >= 0; --i) {
        const QVariantMap z = zones_[i].toMap();
        const QVariantList pts = z.value(QStringLiteral("pts")).toList();
        if (pts.size() < 6) continue;
        QPolygonF poly;
        for (int k = 0; k + 1 < pts.size(); k += 2) poly << QPointF(pts[k].toDouble(), pts[k + 1].toDouble());
        if (poly.containsPoint(n, Qt::OddEvenFill)) return z.value(QStringLiteral("id")).toInt();
    }
    return -1;
}

void VideoItem::finishDraft()
{
    while (draft_.size() >= 2) {
        const QPointF a = draft_[draft_.size() - 1], b = draft_[draft_.size() - 2];
        if (qAbs(a.x() - b.x()) < kDupEps && qAbs(a.y() - b.y()) < kDupEps) draft_.removeLast();
        else break;
    }
    const int id = nextFreeZoneId();
    if (draft_.size() >= 3 && id >= 0) {
        QVariantList pts;
        for (const QPointF& n : draft_) pts << n.x() << n.y();
        selectedZone_ = id;
        emit selectedZoneChanged();
        emit roiCompleted(id, pts);
    }
    setDrawMode(false);
}

void VideoItem::mousePressEvent(QMouseEvent* e)
{
    if (!drawMode_) {
        if (e->button() == Qt::LeftButton && !zones_.isEmpty()) {
            const int hit = zoneAt(toNorm(e->position()));
            setSelectedZone(hit);
            emit zoneClicked(hit);
        }
        return;
    }
    if (e->button() == Qt::RightButton) { finishDraft(); return; }
    if (e->button() == Qt::LeftButton) {
        if (draft_.size() < kMaxPoints) draft_ << toNorm(e->position());
        update();
    }
}

void VideoItem::mouseDoubleClickEvent(QMouseEvent*) { if (drawMode_) finishDraft(); }

void VideoItem::hoverMoveEvent(QHoverEvent* e)
{
    if (!drawMode_) return;
    hover_ = toNorm(e->position());
    hasHover_ = true;
    update();
}

void VideoItem::setAlert(bool on)
{
    if (on == alert_) return;
    alert_ = on;
    if (on) {
        alertBlink_ = true;
        alertTimer_.start();
    } else {
        alertTimer_.stop();
        alertBlink_ = false;
    }
    update();
    emit changed();
}

QRectF VideoItem::displayRect() const
{
    if (frame_.isNull()) return QRectF(0, 0, width(), height());
    QSizeF fs = frame_.size();
    // 비율 유지 + 칸 꽉 채우기(cover), 넘치는 부분은 중앙 크롭 — 위젯과 같음
    fs.scale(QSizeF(width(), height()), Qt::KeepAspectRatioByExpanding);
    return QRectF((width() - fs.width()) / 2.0, (height() - fs.height()) / 2.0,
                  fs.width(), fs.height());
}

QPointF VideoItem::toItem(const QPointF& n) const
{
    const QRectF r = displayRect();
    return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
}

void VideoItem::paint(QPainter* p)
{
    const QRectF all(0, 0, width(), height());
    p->setClipRect(all);
    p->fillRect(all, QColor(QString::fromLatin1(kVideoSurface)));
    p->setFont(baseFont());

    if (frame_.isNull()) {
        p->setPen(QColor(139, 148, 158));
        p->drawText(all, Qt::AlignCenter, QStringLiteral("신호 대기 중…"));
    } else {
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);
        p->drawImage(displayRect(), frame_, frame_.rect());
    }

    p->setRenderHint(QPainter::Antialiasing, true);

    // 침대 ROI 오버레이 (videoview.cpp paintEvent 와 같은 그림)
    if (!zones_.isEmpty() && !frame_.isNull()) {
        QFont zf = baseFont();
        zf.setBold(true);
        zf.setPixelSize(12);
        const QFontMetrics zfm(zf);
        for (const QVariant& zv : zones_) {
            const QVariantMap z = zv.toMap();
            const QVariantList pts = z.value(QStringLiteral("pts")).toList();
            if (pts.size() < 6) continue;
            const int id = z.value(QStringLiteral("id")).toInt();
            const QColor line = kZoneColors[(id < 0 ? 0 : id) % 8];
            const bool sel = (id == selectedZone_);
            QColor fill = line;
            fill.setAlpha(sel ? 120 : 70);
            QPolygonF poly;
            for (int i = 0; i + 1 < pts.size(); i += 2)
                poly << toItem(QPointF(pts[i].toDouble(), pts[i + 1].toDouble()));
            QPainterPath path;
            path.addPolygon(poly);
            path.closeSubpath();
            p->fillPath(path, fill);
            p->setPen(QPen(sel ? line.lighter(135) : line, sel ? 3 : 2));
            p->setBrush(Qt::NoBrush);
            p->drawPolygon(poly);

            const QString text = z.value(QStringLiteral("label")).toString();
            if (text.isEmpty()) continue;
            const QPointF c = poly.boundingRect().center();
            const int tw = zfm.horizontalAdvance(text);
            const int th = zfm.height();
            QRectF chip(c.x() - tw / 2.0 - 7, c.y() - th / 2.0 - 3, tw + 14, th + 6);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 165));
            p->drawRoundedRect(chip, 5, 5);
            p->setFont(zf);
            p->setPen(line.lighter(135));
            p->drawText(chip, Qt::AlignCenter, text);
        }
        p->setFont(baseFont());
        p->setBrush(Qt::NoBrush);
    }

    // 좌상단 상태 배지: ● LIVE  CHn
    {
        const int m = 8;
        QFont lf = baseFont();
        lf.setBold(true);
        p->setFont(lf);
        const QFontMetrics fm(lf);

        const QString st = live_ ? QStringLiteral("LIVE") : QStringLiteral("미연결");
        const QColor dotc = live_ ? QColor(0xFF, 0x5A, 0x5F) : QColor(0x8B, 0x98, 0xA5);
        const QString name = displayName_.isEmpty()
                                 ? QStringLiteral("CH%1").arg(channel_ + 1)
                                 : displayName_;

        const qreal dr = 6;
        const int stW = fm.horizontalAdvance(st);
        const int nameW = fm.horizontalAdvance(name);
        const int boxH = fm.height() + 6;
        const int boxW = 8 + int(dr) + 6 + stW + 10 + nameW + 9;
        QRectF box(m, m, boxW, boxH);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 155));
        p->drawRoundedRect(box, 4, 4);

        p->setBrush(dotc);
        p->drawEllipse(QPointF(box.left() + 8 + dr / 2, box.center().y()), dr / 2, dr / 2);
        qreal tx = box.left() + 8 + dr + 6;
        p->setPen(live_ ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x8B, 0x98, 0xA5));
        p->drawText(QRectF(tx, box.top(), stW + 2, box.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, st);
        tx += stW + 10;
        p->setPen(QColor(0xE6, 0xED, 0xF3));
        p->drawText(QRectF(tx, box.top(), nameW + 2, box.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, name);
    }

    // 그리는 중인 다각형 + 안내 배너
    if (drawMode_) {
        const QColor draftLine(224, 148, 43);
        if (!draft_.isEmpty()) {
            QPolygonF poly;
            for (const auto& n : draft_) poly << toItem(n);
            p->setPen(QPen(draftLine, 2, Qt::DashLine));
            p->setBrush(Qt::NoBrush);
            p->drawPolyline(poly);
            if (hasHover_) p->drawLine(poly.back(), toItem(hover_));
            p->setPen(QPen(QColor(245, 247, 245), 1));
            p->setBrush(draftLine);
            for (const auto& pt : poly) p->drawEllipse(pt, 4, 4);
        }
        const int id = nextFreeZoneId();
        const QString msg = id < 0 ? QStringLiteral("침대가 가득 찼습니다 (최대 %1개)").arg(kMaxRoiZonesQml)
                                   : QStringLiteral("침대 %1 영역 클릭 · 더블클릭(또는 우클릭)으로 완료").arg(id + 1);
        QFont bf = baseFont(); bf.setBold(true); p->setFont(bf);
        const QFontMetrics bfm(bf);
        const int tw = bfm.horizontalAdvance(msg), bh = bfm.height() + 8;
        const int top = 8 + bfm.height() + 6 + 6;
        QRectF banner((width() - tw - 24) / 2.0, top, tw + 24, bh);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 175));
        p->drawRoundedRect(banner, 4, 4);
        p->setPen(draftLine);
        p->drawText(banner, Qt::AlignCenter, msg);
        p->setBrush(Qt::NoBrush);
    }

    if (alert_) {
        const QColor red(229, 83, 60);

        // 낙상 위치 십자선
        if (alertNx_ >= 0 && alertNy_ >= 0 && !frame_.isNull()) {
            const QPointF c = toItem(QPointF(alertNx_, alertNy_));
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(red, 3));
            p->drawEllipse(c, 18, 18);
            p->drawLine(QPointF(c.x() - 26, c.y()), QPointF(c.x() + 26, c.y()));
            p->drawLine(QPointF(c.x(), c.y() - 26), QPointF(c.x(), c.y() + 26));
        }

        if (alertBlink_) {
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(red, 5));
            p->drawRect(all.adjusted(3, 3, -3, -3));

            p->setFont(baseFont());
            const qreal bw = p->fontMetrics().horizontalAdvance(alertLabel_) + 28;
            p->setBrush(red);
            p->setPen(Qt::NoPen);
            QRectF badge((width() - bw) / 2.0, 8, bw, 24);
            p->drawRoundedRect(badge, 6, 6);
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, alertLabel_);
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  TimelineItem — timelinebar.cpp paintEvent() (라이브 모드)
// ═══════════════════════════════════════════════════════════
namespace {
constexpr qint64 kMinute = 60LL * 1000;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kMinSpan = 5 * kMinute;
constexpr qint64 kMaxSpan = 48 * kHour;
constexpr int kRulerH = 18;
}  // namespace

TimelineItem::TimelineItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(QCursor(Qt::PointingHandCursor));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    startMs_ = now - 12 * kHour;   // 기본 창 = NVR 보존기간(12시간)
    endMs_ = now;
    followTimer_.setInterval(1000);
    connect(&followTimer_, &QTimer::timeout, this, [this] {
        if (!liveMode_) return;
        const qint64 span = endMs_ - startMs_;
        endMs_ = QDateTime::currentMSecsSinceEpoch();
        startMs_ = endMs_ - span;
        update();
    });
    followTimer_.start();
}

void TimelineItem::addMarker(qint64 atMs, const QColor& color)
{
    markers_.append({atMs, color});
    update();
}

void TimelineItem::setSpans(const QVariantList& spans)
{
    spans_.clear();
    for (const QVariant& v : spans) {
        const QVariantList a = v.toList();
        if (a.size() >= 2) spans_.append({a[0].toLongLong(), a[1].toLongLong()});
    }
    update();
}

void TimelineItem::setMarkers(const QVariantList& markers)
{
    markers_.clear();
    for (const QVariant& v : markers) {
        const QVariantMap m = v.toMap();
        markers_.append({m.value(QStringLiteral("atMs")).toLongLong(), QColor(m.value(QStringLiteral("color")).toString())});
    }
    update();
}

void TimelineItem::followNow()
{
    const qint64 span = endMs_ - startMs_;
    endMs_ = QDateTime::currentMSecsSinceEpoch();
    startMs_ = endMs_ - span;
    update();
}

qint64 TimelineItem::msForX(qreal x) const
{
    if (width() <= 0) return startMs_;
    const qreal t = qBound(0.0, x / width(), 1.0);
    return startMs_ + qint64(t * (endMs_ - startMs_));
}

void TimelineItem::mousePressEvent(QMouseEvent* e)
{
    dragging_ = true;
    const qint64 ms = msForX(e->position().x());
    setPlayhead(double(ms));
    emit seekRequested(double(ms));
}

void TimelineItem::mouseMoveEvent(QMouseEvent* e)
{
    hoverX_ = e->position().x();
    if (dragging_) {
        const qint64 ms = msForX(hoverX_);
        setPlayhead(double(ms));
        emit seekRequested(double(ms));
    }
    update();
}

void TimelineItem::mouseReleaseEvent(QMouseEvent* e)
{
    if (!dragging_) return;
    dragging_ = false;
    emit seekCommitted(double(msForX(e->position().x())));
}

void TimelineItem::hoverMoveEvent(QHoverEvent* e) { hoverX_ = e->position().x(); update(); }
void TimelineItem::hoverLeaveEvent(QHoverEvent*) { hoverX_ = -1; update(); }

qreal TimelineItem::xForMs(qint64 ms) const
{
    const qint64 span = endMs_ - startMs_;
    if (span <= 0) return 0;
    return qreal(ms - startMs_) / qreal(span) * width();
}

qint64 TimelineItem::tickStepMs() const
{
    const qint64 span = endMs_ - startMs_;
    if (span <= 1 * kHour) return 5 * kMinute;
    if (span <= 3 * kHour) return 15 * kMinute;
    if (span <= 6 * kHour) return 30 * kMinute;
    if (span <= 14 * kHour) return kHour;
    if (span <= 30 * kHour) return 2 * kHour;
    return 4 * kHour;
}

void TimelineItem::paint(QPainter* p)
{
    p->setRenderHint(QPainter::Antialiasing, false);
    const QColor bg(QString::fromLatin1(kCard));
    const QColor border(QString::fromLatin1(kBorder));
    const QColor sub(QString::fromLatin1(kTextSub));
    const QColor sel(QString::fromLatin1(kSelect));
    const qreal W = width(), H = height();

    p->fillRect(QRectF(0, 0, W, H), bg);

    QFont tf = baseFont(11);
    p->setFont(tf);
    const QFontMetrics fm(tf);
    const qint64 step = tickStepMs();
    const int tzOffsetMs = QDateTime::currentDateTime().offsetFromUtc() * 1000;
    qint64 t = ((startMs_ + tzOffsetMs) / step) * step - tzOffsetMs;
    if (t < startMs_) t += step;
    for (; t <= endMs_; t += step) {
        const qreal x = xForMs(t);
        p->setPen(QPen(border, 1));
        p->drawLine(QPointF(x, kRulerH - 4), QPointF(x, H - 8));
        const QString label = QDateTime::fromMSecsSinceEpoch(t).toString("HH:mm");
        const int w = fm.horizontalAdvance(label);
        p->setPen(sub);
        p->drawText(QRectF(x - w / 2.0 - 4, 1, w + 8, kRulerH - 4), Qt::AlignCenter, label);
    }

    const QRectF track(0, kRulerH + 6, W, H - kRulerH - 6 - 8);
    QColor trackBg = border;
    trackBg.setAlpha(90);
    p->fillRect(track, trackBg);

    const QColor rec(QString::fromLatin1(kNormal));
    for (const auto& sp : spans_) {
        if (sp.endMs <= startMs_ || sp.startMs >= endMs_) continue;
        const qreal x1 = xForMs(qMax(sp.startMs, startMs_));
        const qreal x2 = xForMs(qMin(sp.endMs, endMs_));
        p->fillRect(QRectF(x1, track.top(), qMax(1.0, x2 - x1), track.height()), rec);
    }

    for (const auto& m : markers_) {
        if (m.atMs < startMs_ || m.atMs > endMs_) continue;
        const qreal x = xForMs(m.atMs);
        p->setPen(QPen(m.color, 2));
        p->drawLine(QPointF(x, track.top() - 3), QPointF(x, track.bottom() + 3));
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now >= startMs_ && now <= endMs_) {
        const qreal x = xForMs(now);
        p->setPen(QPen(liveMode_ ? sel : sub, liveMode_ ? 2 : 1, Qt::DashLine));
        p->drawLine(QPointF(x, kRulerH - 2), QPointF(x, H - 6));
    }

    if (!liveMode_ && playheadMs_ >= startMs_ && playheadMs_ <= endMs_) {
        const qreal x = xForMs(playheadMs_);
        p->setPen(QPen(sel, 2));
        p->drawLine(QPointF(x, kRulerH - 6), QPointF(x, H - 4));
        const QString label = QDateTime::fromMSecsSinceEpoch(playheadMs_).toString("MM-dd HH:mm:ss");
        const int w = fm.horizontalAdvance(label) + 12;
        const qreal bx = qBound(0.0, x - w / 2.0, qreal(W - w));
        const QRectF bubble(bx, H - 16, w, 15);
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(Qt::NoPen);
        p->setBrush(sel);
        p->drawRoundedRect(bubble, 3, 3);
        p->setPen(QColor(QString::fromLatin1(kOnSelect)));
        p->drawText(bubble, Qt::AlignCenter, label);
        p->setRenderHint(QPainter::Antialiasing, false);
    }

    if (hoverX_ >= 0 && !dragging_) {
        p->setPen(QPen(sub, 1));
        p->drawLine(QPointF(hoverX_, kRulerH), QPointF(hoverX_, H - 8));
    }

    p->setPen(QPen(border, 1));
    p->drawLine(QPointF(0, 0), QPointF(W, 0));
}

void TimelineItem::wheelEvent(QWheelEvent* e)
{
    const int steps = e->angleDelta().y();
    if (steps == 0) return;
    const qint64 span = endMs_ - startMs_;
    const qreal factor = steps > 0 ? 0.8 : 1.25;
    const qint64 newSpan = qBound(kMinSpan, qint64(span * factor), kMaxSpan);
    if (newSpan == span) return;
    const qreal t = qBound(0.0, e->position().x() / qMax(1.0, width()), 1.0);
    const qint64 anchor = startMs_ + qint64(t * span);
    startMs_ = anchor - qint64(t * newSpan);
    endMs_ = startMs_ + newSpan;
    update();
}

// ═══════════════════════════════════════════════════════════
//  SparklineItem — sparkline.cpp (범위 40~140, 기준선 4개)
// ═══════════════════════════════════════════════════════════
SparklineItem::SparklineItem(QQuickItem* parent)
    : QQuickPaintedItem(parent), color_(QString::fromLatin1(kTextSub))
{
    setAntialiasing(true);
}

void SparklineItem::addValue(double v)
{
    values_.append(v);
    while (values_.size() > 60) values_.removeFirst();
    update();
}

QVariantList SparklineItem::values() const
{
    QVariantList l;
    for (double d : values_) l << d;
    return l;
}

void SparklineItem::setValues(const QVariantList& v)
{
    values_.clear();
    for (const QVariant& x : v) values_.append(x.toDouble());
    update();
    emit changed();
}

void SparklineItem::paint(QPainter* p)
{
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(0, 0, width(), height()).adjusted(1, 3, -1, -3);
    const double lo = 40, hi = 140, span = hi - lo;
    auto yAt = [&](double v) {
        v = qBound(lo, v, hi);
        return r.bottom() - (v - lo) / span * r.height();
    };

    const QPair<double, QColor> guides[4] = {
        {110.0, QColor(QString::fromLatin1(kCritical))},
        {100.0, QColor(QString::fromLatin1(kWarn))},
        { 55.0, QColor(QString::fromLatin1(kWarn))},
        { 45.0, QColor(QString::fromLatin1(kCritical))},
    };
    for (const auto& g : guides) {
        QColor c = g.second;
        c.setAlpha(110);
        p->setPen(QPen(c, 1.0, Qt::DashLine));
        const double gy = yAt(g.first);
        p->drawLine(QPointF(r.left(), gy), QPointF(r.right(), gy));
    }

    const int n = values_.size();
    if (n < 2) return;
    auto xAt = [&](int i) { return r.left() + r.width() * i / double(n - 1); };
    QPainterPath line;
    for (int i = 0; i < n; ++i) {
        if (i == 0) line.moveTo(xAt(i), yAt(values_[i]));
        else        line.lineTo(xAt(i), yAt(values_[i]));
    }
    QPainterPath fill = line;
    fill.lineTo(xAt(n - 1), r.bottom());
    fill.lineTo(xAt(0), r.bottom());
    fill.closeSubpath();
    QColor fillc = color_;
    fillc.setAlpha(38);
    p->fillPath(fill, fillc);
    p->setPen(QPen(color_, 1.6));
    p->setBrush(Qt::NoBrush);
    p->drawPath(line);
    p->setPen(Qt::NoPen);
    p->setBrush(color_);
    p->drawEllipse(QPointF(xAt(n - 1), yAt(values_.back())), 2.4, 2.4);
}

// ═══════════════════════════════════════════════════════════
//  AlarmOverlayItem — mainwindow_p.h AlarmOverlay
// ═══════════════════════════════════════════════════════════
AlarmOverlayItem::AlarmOverlayItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setVisible(false);
    connect(&timer_, &QTimer::timeout, this, [this] {
        phase_ += 0.10;                       // 느리고 잔잔한 호흡(≈2초 주기)
        if (phase_ > 6.28318) phase_ -= 6.28318;
        update();
    });
}

void AlarmOverlayItem::setActive(bool on)
{
    if (on == active_) return;
    active_ = on;
    setVisible(on);
    if (on) timer_.start(33);
    else    timer_.stop();
    emit changed();
}

void AlarmOverlayItem::paint(QPainter* p)
{
    p->setRenderHint(QPainter::Antialiasing, true);
    const double t = (std::sin(phase_) + 1.0) / 2.0;
    const int a = int((0.20 + 0.30 * t) * 255.0);
    const QColor edge(216, 40, 54, a);
    const QColor clear(216, 40, 54, 0);
    const int W = int(width()), H = int(height());
    const int g = qMin(110, qMin(W, H) / 5);

    auto band = [&](const QRect& r, const QPointF& from, const QPointF& to) {
        QLinearGradient lg(from, to);
        lg.setColorAt(0.0, edge);
        lg.setColorAt(1.0, clear);
        p->fillRect(r, lg);
    };
    band(QRect(0, 0, W, g),     QPointF(0, 0), QPointF(0, g));
    band(QRect(0, H - g, W, g), QPointF(0, H), QPointF(0, H - g));
    band(QRect(0, 0, g, H),     QPointF(0, 0), QPointF(g, 0));
    band(QRect(W - g, 0, g, H), QPointF(W, 0), QPointF(W - g, 0));

    p->setPen(QPen(QColor(230, 55, 66, int(a * 0.8)), 1));
    p->setBrush(Qt::NoBrush);
    p->drawRect(QRectF(0, 0, W, H).adjusted(0.5, 0.5, -0.5, -0.5));
}

// ═══════════════════════════════════════════════════════════
//  BackdropItem — logindialog.cpp makePlaceholder() + paintEvent() 베일
// ═══════════════════════════════════════════════════════════
void BackdropItem::paint(QPainter* p)
{
    p->setRenderHint(QPainter::Antialiasing, true);
    const QSizeF size(width(), height());
    p->fillRect(QRectF(0, 0, size.width(), size.height()), QColor(QLatin1String(kDark.bgDeep)));
    p->setPen(Qt::NoPen);
    struct Blob { qreal cx, cy, r; const char* color; int alpha; };
    const Blob blobs[] = {
        {0.14, 0.18, 0.62, kDark.accent, 165},   // 좌상단 청록 — 브랜드 색
        {0.88, 0.12, 0.50, kDark.info,   150},   // 우상단 파랑
        {0.80, 0.86, 0.55, kDark.select, 130},   // 우하단 주황
        {0.30, 0.92, 0.45, kDark.bgDeep, 120},   // 좌하단은 눌러 깊이감
    };
    for (const Blob& b : blobs) {
        const QPointF c(size.width() * b.cx, size.width() * b.cy * (size.height() / size.width()));
        const qreal r = size.width() * b.r;
        QColor color(QLatin1String(b.color));
        QRadialGradient g(c, r);
        color.setAlpha(b.alpha);
        g.setColorAt(0.0, color);
        color.setAlpha(0);
        g.setColorAt(1.0, color);
        p->setBrush(g);
        p->drawEllipse(c, r, r);
    }
    QLinearGradient veil(0, 0, 0, size.height());
    veil.setColorAt(0.0, QColor(10, 15, 21, 55));
    veil.setColorAt(1.0, QColor(10, 15, 21, 135));
    p->fillRect(QRectF(0, 0, size.width(), size.height()), veil);
}

// ═══════════════════════════════════════════════════════════
//  IconProvider
// ═══════════════════════════════════════════════════════════
QPixmap IconProvider::requestPixmap(const QString& id, QSize* size, const QSize& requestedSize)
{
    // id = "nav/<종류>/<색>" 또는 "tool/<종류>/<색>"  (색은 #없는 hex)
    const QStringList parts = id.split(QLatin1Char('/'));
    const QString set = parts.value(0);
    const int kind = parts.value(1).toInt();
    const QColor color(QLatin1Char('#') + parts.value(2, QStringLiteral("8B98A5")));
    const bool tool = set == QLatin1String("tool");
    const int px = requestedSize.width() > 0 ? requestedSize.width() : (tool ? 15 : 20);
    const QPixmap pm = tool ? tileToolIconPixmap(kind, color, px) : navIconPixmap(kind, color, px);
    if (size) *size = pm.size();
    return pm;
}

// ── ActivityChartItem (위젯 ActivityChart::paintEvent 와 같은 그리기) ──
QVariantList ActivityChartItem::steps() const
{
    QVariantList l;
    for (int v : steps_) l << v;
    return l;
}

void ActivityChartItem::setSteps(const QVariantList& v)
{
    steps_ = QVector<int>(24, 0);
    hasData_ = false;
    for (int i = 0; i < 24 && i < v.size(); ++i) {
        steps_[i] = v[i].toInt();
        if (steps_[i] > 0) hasData_ = true;
    }
    update();
    emit changed();
}

void ActivityChartItem::paint(QPainter* p)
{
    constexpr int kPadL = 44, kPadR = 14, kPadT = 14, kPadB = 26;
    p->setRenderHint(QPainter::Antialiasing, true);
    const QColor accent(QString::fromLatin1(kAccent)), sub(QString::fromLatin1(kTextSub)),
                 border(QString::fromLatin1(kBorder));
    if (!hasData_) {
        p->setPen(sub);
        QFont f = p->font(); f.setPixelSize(13); p->setFont(f);
        p->drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter, QStringLiteral("이 날짜의 활동 기록이 없습니다"));
        return;
    }
    const QRectF plot(kPadL, kPadT, qMax(10.0, width() - kPadL - kPadR), qMax(10.0, height() - kPadT - kPadB));
    int stepMax = 0;
    for (int v : steps_) stepMax = qMax(stepMax, v);
    int step = 100;
    while (stepMax > step * 5) step *= 2;
    const int axisMax = ((stepMax + step - 1) / step) * step;
    QFont small = p->font(); small.setPixelSize(11); p->setFont(small);
    const QFontMetrics fm(small);
    p->setPen(QPen(border, 1, Qt::DotLine));
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        p->drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p->setPen(sub);
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        const QString t = QString::number(axisMax * g / 2);
        p->drawText(QPointF(plot.left() - 6 - fm.horizontalAdvance(t), y + fm.ascent() / 2.0 - 1), t);
    }
    const double slot = plot.width() / 24, barW = qMax(2.0, slot * 0.62);
    for (int h = 0; h < 24; ++h) {
        if (steps_[h] <= 0) continue;
        const double bh = plot.height() * steps_[h] / double(axisMax);
        QColor c = accent;
        c.setAlpha(steps_[h] == stepMax ? 255 : 150);
        p->fillRect(QRectF(plot.left() + slot * h + (slot - barW) / 2.0, plot.bottom() - bh, barW, bh), c);
    }
    p->setPen(sub);
    for (int h = 0; h < 24; h += 3) {
        const QString t = QString::number(h);
        p->drawText(QPointF(plot.left() + slot * h + slot / 2.0 - fm.horizontalAdvance(t) / 2.0, plot.bottom() + fm.ascent() + 5), t);
    }
}
