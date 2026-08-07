#include "clickslider.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <algorithm>

#include "theme.h"

ClickSlider::ClickSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
    setRange(0, 100);
    setMinimumHeight(34);
    setCursor(Qt::PointingHandCursor);

    // 우측 kValueMargin은 값 숫자용으로 비워 두고(padding-right), groove는 테마색.
    //  · groove(전체 트랙): border 색 / sub-page(채워진 부분): accent / handle: 본문색
    setStyleSheet(QString(R"(
        QSlider { padding-right: %1px; }
        QSlider::groove:horizontal {
            height: 4px; border-radius: 2px; background: %2;
        }
        QSlider::sub-page:horizontal {
            height: 4px; border-radius: 2px; background: %3;
        }
        QSlider::handle:horizontal {
            width: 12px; height: 16px; margin: -7px 0; border-radius: 3px; background: %4;
        }
        QSlider::handle:horizontal:hover { background: %3; }
    )")
                      .arg(kValueMargin)
                      .arg(kBorder)
                      .arg(kAccent)
                      .arg(kTextMain));
}

void ClickSlider::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // 값 숫자 영역을 뺀 실제 트랙 폭 기준으로 클릭 비율 계산.
        const double track = std::max(1, width() - kValueMargin);
        const double ratio = std::clamp(e->position().x() / track, 0.0, 1.0);
        setValue(minimum() + static_cast<int>(ratio * (maximum() - minimum()) + 0.5));
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

void ClickSlider::paintEvent(QPaintEvent* e) {
    QSlider::paintEvent(e);  // 기본 슬라이더 먼저 그리고
    QPainter p(this);
    p.setPen(QColor(kTextMain));
    QFont f = font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(width() - (kValueMargin - 4), 0, kValueMargin - 8, height()),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(value()));
}
