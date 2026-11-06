#ifndef UIICONS_H
#define UIICONS_H

// 좌측 네비·타일 툴바 선 아이콘 — 위젯 앱과 QML 앱이 같은 그림을 쓰도록 한 곳에 둔다.
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

// 좌측 네비 아이콘 — 외부 리소스 없이 QPainter로 그린다.
// 이모지를 쓰면 폰트마다 톤·크기가 제각각이라 관제 UI에서 장난감처럼 보인다.
// 선 굵기 하나로 통일된 단색 아이콘이라 팔레트가 바뀌면 색만 다시 입히면 된다.
inline QPixmap navIconPixmap(int kind, const QColor& c, int px = 20)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal m = 2.5, w = px - 2 * m;
    switch (kind) {
        case 0: {   // 실시간 관제 — 2×2 영상 그리드
            const qreal g = 1.8, s = (w - g) / 2.0;
            for (int r = 0; r < 2; ++r)
                for (int col = 0; col < 2; ++col)
                    p.drawRoundedRect(
                        QRectF(m + col * (s + g), m + r * (s + g), s, s), 1.5, 1.5);
            break;
        }
        case 1: {   // 이벤트 기록 — 점 + 줄 목록
            for (int i = 0; i < 3; ++i) {
                const qreal y = m + 1.5 + i * (w - 3) / 2.0;
                p.setBrush(c);
                p.drawEllipse(QPointF(m + 1.2, y), 1.15, 1.15);
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(m + 5.0, y), QPointF(m + w, y));
            }
            break;
        }
        case 2: {   // 영상 검색 — 돋보기
            const qreal r = w * 0.32;
            const QPointF center(m + w * 0.38, m + w * 0.38);
            p.drawEllipse(center, r, r);
            p.drawLine(QPointF(center.x() + r * 0.72, center.y() + r * 0.72),
                       QPointF(m + w, m + w));
            break;
        }
        case 3: {   // 일일 리포트 — 문서(가로줄)
            p.drawRoundedRect(QRectF(m, m, w, w), 2.0, 2.0);
            for (int i = 0; i < 3; ++i) {
                const qreal y = m + w * 0.30 + i * w * 0.22;
                p.drawLine(QPointF(m + w * 0.2, y), QPointF(m + w * 0.8, y));
            }
            break;
        }
        case 4: {   // 입소자 관리 — 사람
            p.drawEllipse(QRectF(m + w * 0.29, m + w * 0.04, w * 0.42, w * 0.42));
            p.drawArc(QRectF(m + w * 0.06, m + w * 0.54, w * 0.88, w * 0.84), 0, 180 * 16);
            break;
        }
        default: {  // 장치 설정(5) — 카메라 바디 + 렌즈
            p.drawRoundedRect(QRectF(m, m + w * 0.20, w, w * 0.62), 2.5, 2.5);
            p.drawEllipse(QRectF(m + w * 0.33, m + w * 0.36, w * 0.34, w * 0.32));
            p.drawLine(QPointF(m + w * 0.30, m + w * 0.20),
                       QPointF(m + w * 0.40, m + w * 0.06));
            p.drawLine(QPointF(m + w * 0.40, m + w * 0.06),
                       QPointF(m + w * 0.62, m + w * 0.06));
            break;
        }
    }
    return pm;
}

// 타일 호버 툴바용 16px 선 아이콘. 유니코드 글리프(⛶ ▣ ✎ ⤓)를 쓰면 폰트에 따라
// 이모지로 대체돼 색이 튀거나 아예 두부(□)로 나온다 — 직접 그려서 모양과 색을
// 두 테마·모든 환경에서 고정한다.
inline QPixmap tileToolIconPixmap(int kind, const QColor& c, int px = 15)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal m = 2.0, w = px - 2 * m;
    if (kind == 0) {   // 크게 보기 — 네 모서리 꺾쇠
        const qreal a = w * 0.34;
        p.drawPolyline(QPolygonF({QPointF(m, m + a), QPointF(m, m), QPointF(m + a, m)}));
        p.drawPolyline(QPolygonF({QPointF(m + w - a, m), QPointF(m + w, m),
                                  QPointF(m + w, m + a)}));
        p.drawPolyline(QPolygonF({QPointF(m + w, m + w - a), QPointF(m + w, m + w),
                                  QPointF(m + w - a, m + w)}));
        p.drawPolyline(QPolygonF({QPointF(m + a, m + w), QPointF(m, m + w),
                                  QPointF(m, m + w - a)}));
    } else {           // 스냅샷 — 아래로 향한 화살표 + 받침
        const qreal cx = m + w / 2.0;
        p.drawLine(QPointF(cx, m), QPointF(cx, m + w * 0.62));
        p.drawPolyline(QPolygonF({QPointF(cx - w * 0.22, m + w * 0.40), QPointF(cx, m + w * 0.66),
                                  QPointF(cx + w * 0.22, m + w * 0.40)}));
        p.drawLine(QPointF(m, m + w), QPointF(m + w, m + w));
    }
    return pm;
}

#endif // UIICONS_H
