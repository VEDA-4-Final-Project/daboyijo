#include "sparkline.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>

Sparkline::Sparkline(QWidget* parent) : QWidget(parent) {
    // v2: 세로 Expanding을 뺐다. 남는 공간을 그래프가 흡수하면 바이탈 타일이
    // 부풀어 우측 레일이 영상 폭을 잡아먹는다 — 실제로 그렇게 됐다.
    // 추세는 "흐름만 보이면 되는" 보조 정보라 34px 고정이면 충분하다.
    setFixedHeight(34);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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

    const int n = values_.size();

    // v2: 데이터가 없으면 가이드 점선도 그리지 않는다.
    // v1은 guides_를 먼저 그리고 그 뒤에 n<2로 반환해서, 신호가 없을 때
    // "점선 4줄만 뜬 빈 상자"가 됐다 — 화면에서 고장난 칸처럼 읽혔다.
    // 대신 중앙에 옅은 기준선 하나만 그어 "자리는 있으나 신호가 없다"를 표현한다.
    if (n < 2) {
        QColor idle = color_;
        idle.setAlpha(46);
        p.setPen(QPen(idle, 1.0, Qt::DotLine));
        const double my = r.center().y();
        p.drawLine(QPointF(r.left(), my), QPointF(r.right(), my));
        return;
    }

    // 기준선(주의/위험 임계) — 점선, 값 선보다 아래에 먼저 그린다.
    for (const auto& g : guides_) {
        QColor c = g.second;
        c.setAlpha(110);
        QPen pen(c, 1.0, Qt::DashLine);
        p.setPen(pen);
        const double gy = yAt(g.first);
        p.drawLine(QPointF(r.left(), gy), QPointF(r.right(), gy));
    }

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