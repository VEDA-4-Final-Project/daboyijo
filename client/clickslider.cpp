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
    // Phase 1(SCREEN-04) QSS 수렴에서 이 시트는 의도적으로 제외했다 —
    // kValueMargin이 mousePressEvent()/paintEvent()의 클릭 좌표·텍스트 위치 계산에도
    // 쓰이는 C++ 레이아웃 상수라, QSS 파일로 분리하면 두 곳에서 값이 따로 관리되어
    // 어긋나는 순간 슬라이더 클릭 위치가 조용히 틀어진다.
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
        /* 비활성 — 못 만지는 상태가 눈에 보여야 한다. QSS를 이 위젯이 직접 들고
           있어서 앱 전역 시트의 :disabled 규칙이 여기까지 닿지 않는다. */
        QSlider::sub-page:horizontal:disabled { background: %2; }
        QSlider::handle:horizontal:disabled { background: %5; }
    )")
                      .arg(kValueMargin)
                      .arg(kBorder)
                      .arg(kAccent)
                      .arg(kTextMain)
                      .arg(kTextSub));
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
    // 값 숫자도 비활성이면 죽인다 — 트랙만 흐려지고 숫자가 또렷하면 반쯤 살아
    // 있는 것처럼 보인다.
    p.setPen(QColor(isEnabled() ? kTextMain : kTextSub));
    QFont f = font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(width() - (kValueMargin - 4), 0, kValueMargin - 8, height()),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(value()));
}
