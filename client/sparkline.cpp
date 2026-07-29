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

void Sparkline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int n = values_.size();
    if (n < 2) return;

    const QRectF r = rect().adjusted(1, 3, -1, -3);
    double lo = *std::min_element(values_.begin(), values_.end());
    double hi = *std::max_element(values_.begin(), values_.end());
    if (hi - lo < 1e-6) { lo -= 1.0; hi += 1.0; }  // 평평하면 살짝 벌려 중앙에
    const double span = hi - lo;

    auto xAt = [&](int i) { return r.left() + r.width() * i / double(n - 1); };
    auto yAt = [&](double v) { return r.bottom() - (v - lo) / span * r.height(); };

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
