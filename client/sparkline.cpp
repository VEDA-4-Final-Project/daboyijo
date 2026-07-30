#include "sparkline.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>

Sparkline::Sparkline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(34);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);  // 클릭 방해 안 하게
}

void Sparkline::addValue(double v) {
    values_.append(v);
    while (values_.size() > capacity_) values_.removeFirst();
    update();
}

void Sparkline::setLineColor(const QColor& c) {
    if (color_ == c) return;
    color_ = c;
    update();
}

void Sparkline::clear() {
    values_.clear();
    update();
}

void Sparkline::setRange(double lo, double hi) {
    fixedRange_ = true;
    rangeLo_ = lo;
    rangeHi_ = hi;
    update();
}

void Sparkline::setGuides(const QVector<QPair<double, QColor>>& guides) {
    guides_ = guides;
    update();
}

void Sparkline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect().adjusted(1, 3, -1, -3);

    // Y 범위: 고정이면 그대로, 아니면 최근값 min~max 자동.
    double lo, hi;
    if (fixedRange_) {
        lo = rangeLo_;
        hi = rangeHi_;
    } else {
        if (values_.size() < 2) return;
        lo = *std::min_element(values_.begin(), values_.end());
        hi = *std::max_element(values_.begin(), values_.end());
        if (hi - lo < 1e-6) { lo -= 1.0; hi += 1.0; }
    }
    const double span = (hi - lo > 1e-6) ? (hi - lo) : 1.0;
    auto yAt = [&](double v) {
        v = qBound(lo, v, hi);
        return r.bottom() - (v - lo) / span * r.height();
    };

    // 기준선(주의/위험 임계) — 점선, 값 선보다 아래에 먼저 그린다.
    for (const auto& g : guides_) {
        QColor c = g.second;
        c.setAlpha(110);
        QPen pen(c, 1.0, Qt::DashLine);
        p.setPen(pen);
        const double gy = yAt(g.first);
        p.drawLine(QPointF(r.left(), gy), QPointF(r.right(), gy));
    }

    const int n = values_.size();
    if (n < 2) return;

    auto xAt = [&](int i) { return r.left() + r.width() * i / double(n - 1); };

    QPainterPath line;
    for (int i = 0; i < n; ++i) {
        const double x = xAt(i), y = yAt(values_[i]);
        if (i == 0) line.moveTo(x, y);
        else        line.lineTo(x, y);
    }

    // 선 아래 옅은 채움
    QPainterPath fill = line;
    fill.lineTo(xAt(n - 1), r.bottom());
    fill.lineTo(xAt(0), r.bottom());
    fill.closeSubpath();
    QColor fillc = color_;
    fillc.setAlpha(38);
    p.fillPath(fill, fillc);

    // 추세선
    p.setPen(QPen(color_, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);

    // 최신 점 강조
    p.setPen(Qt::NoPen);
    p.setBrush(color_);
    p.drawEllipse(QPointF(xAt(n - 1), yAt(values_.back())), 2.4, 2.4);
}
