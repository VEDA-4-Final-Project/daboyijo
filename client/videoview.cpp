#include "videoview.h"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace {
// ROI 오버레이 색 (브랜드 강조색 힐링 그린과 맞춤 · 어두운 영상 위라 밝게)
const QColor kRoiLine(47, 158, 143);          // 힐링 그린 외곽선
const QColor kRoiFill(47, 158, 143, 70);      // 반투명 채움
const QColor kDraftLine(224, 148, 43);        // 그리는 중(주황)
const QColor kVertex(245, 247, 245);          // 꼭짓점 마커
constexpr double kDupEps = 0.006;             // 더블클릭 중복 꼭짓점 제거 임계
constexpr int kMaxPoints = 32;                // 프로토콜 DBJ_ROI_MAX_POINTS와 동일
}  // namespace

VideoView::VideoView(int channel, QWidget* parent)
    : QWidget(parent), channel_(channel) {
    setMouseTracking(true);  // 그리기 미리보기 선을 위해 이동 이벤트 항상 수신
    setMinimumHeight(160);
    setAttribute(Qt::WA_OpaquePaintEvent);  // 매 프레임 전체를 다시 그림
}

void VideoView::setFrame(const QPixmap& frame) {
    frame_ = frame;
    update();
}

void VideoView::setRoi(const QPolygonF& normPts) {
    roi_ = normPts;
    update();
}

void VideoView::clearRoi() {
    roi_.clear();
    update();
}

void VideoView::setDrawMode(bool on) {
    if (drawMode_ == on) return;
    drawMode_ = on;
    draft_.clear();
    hasHover_ = false;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    emit drawModeChanged(channel_, on);
    update();
}

void VideoView::cancelDraft() {
    if (!drawMode_) return;
    setDrawMode(false);
}

void VideoView::setRoiVisible(bool on) {
    roiVisible_ = on;
    update();
}

void VideoView::setAlert(bool on, const QPointF& normPt) {
    alertPt_ = normPt;
    if (alert_ != on) {
        alert_ = on;
        if (on) {
            alertBlink_ = true;
            if (!alertTimer_) {
                alertTimer_ = new QTimer(this);
                alertTimer_->setInterval(450);
                connect(alertTimer_, &QTimer::timeout, this, [this]() {
                    alertBlink_ = !alertBlink_;
                    update();
                });
            }
            alertTimer_->start();
        } else {
            if (alertTimer_) alertTimer_->stop();
            alertBlink_ = false;
        }
    }
    update();
}

// ── 좌표 변환 ────────────────────────────────────────────────
QRectF VideoView::displayRect() const {
    if (frame_.isNull()) return QRectF(rect());
    QSizeF fs = frame_.size();
    fs.scale(size(), Qt::KeepAspectRatio);  // 비율 유지로 위젯에 맞춤
    const qreal x = (width() - fs.width()) / 2.0;
    const qreal y = (height() - fs.height()) / 2.0;
    return QRectF(x, y, fs.width(), fs.height());
}

QPointF VideoView::toNorm(const QPointF& w) const {
    const QRectF r = displayRect();
    if (r.width() <= 0 || r.height() <= 0) return QPointF(0, 0);
    const qreal nx = (w.x() - r.left()) / r.width();
    const qreal ny = (w.y() - r.top()) / r.height();
    return QPointF(qBound(0.0, nx, 1.0), qBound(0.0, ny, 1.0));
}

QPointF VideoView::toWidget(const QPointF& n) const {
    const QRectF r = displayRect();
    return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
}

// ── 그리기 ───────────────────────────────────────────────────
void VideoView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (frame_.isNull()) {
        p.setPen(QColor(139, 148, 158));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("신호 없음"));
    } else {
        p.drawPixmap(displayRect(), frame_, frame_.rect());
    }

    p.setRenderHint(QPainter::Antialiasing, true);

    // 확정된 ROI (모니터링 오버레이)
    if (roiVisible_ && roi_.size() >= 2) {
        QPolygonF poly;
        for (const auto& n : roi_) poly << toWidget(n);
        QPainterPath path;
        path.addPolygon(poly);
        path.closeSubpath();
        p.fillPath(path, kRoiFill);
        p.setPen(QPen(kRoiLine, 2));
        p.drawPolygon(poly);
    }

    // 그리는 중인 다각형
    if (drawMode_ && !draft_.empty()) {
        QPolygonF poly;
        for (const auto& n : draft_) poly << toWidget(n);

        p.setPen(QPen(kDraftLine, 2, Qt::DashLine));
        p.drawPolyline(poly);
        // 커서까지 미리보기 선
        if (hasHover_) p.drawLine(poly.back(), toWidget(hover_));

        // 꼭짓점 마커
        p.setPen(QPen(kVertex, 1));
        p.setBrush(kDraftLine);
        for (const auto& pt : poly) p.drawEllipse(pt, 4, 4);
    }

    // 그리기 모드 안내 배너
    if (drawMode_) {
        p.setPen(kDraftLine);
        p.drawText(QRectF(0, 6, width(), 20), Qt::AlignHCenter,
                   QStringLiteral("침대 영역 클릭 · 더블클릭(또는 우클릭)으로 완료"));
    }

    // 낙상 경보 — 발생 위치 마커(상시) + 빨간 테두리/배지(점멸)
    if (alert_) {
        const QColor red(229, 83, 60);

        // 서버가 보낸 발생 위치(x,y)에 십자 마커
        if (alertPt_.x() >= 0 && alertPt_.y() >= 0 && !frame_.isNull()) {
            const QPointF c = toWidget(alertPt_);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(red, 3));
            p.drawEllipse(c, 18, 18);
            p.drawLine(QPointF(c.x() - 26, c.y()), QPointF(c.x() + 26, c.y()));
            p.drawLine(QPointF(c.x(), c.y() - 26), QPointF(c.x(), c.y() + 26));
        }

        // 테두리 + 상단 배지 점멸
        if (alertBlink_) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(red, 5));
            p.drawRect(rect().adjusted(3, 3, -3, -3));

            p.setBrush(red);
            p.setPen(Qt::NoPen);
            QRectF badge((width() - 130) / 2.0, 8, 130, 24);
            p.drawRoundedRect(badge, 6, 6);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, QStringLiteral("🚨 낙상 감지"));
        }
    }
}

void VideoView::mousePressEvent(QMouseEvent* e) {
    if (!drawMode_) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (e->button() == Qt::RightButton) {
        finishDraft();  // 우클릭 = 완료
        return;
    }
    if (e->button() == Qt::LeftButton) {
        if (draft_.size() < kMaxPoints) draft_ << toNorm(e->position());
        update();
    }
}

void VideoView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!drawMode_) {
        QWidget::mouseDoubleClickEvent(e);
        return;
    }
    finishDraft();  // 더블클릭 = 완료 (직전 좌클릭 중복점은 finishDraft가 정리)
}

void VideoView::mouseMoveEvent(QMouseEvent* e) {
    if (drawMode_) {
        hover_ = toNorm(e->position());
        hasHover_ = true;
        update();
    }
    QWidget::mouseMoveEvent(e);
}

void VideoView::finishDraft() {
    // 더블클릭 직전 좌클릭이 만든 (거의) 중복 꼭짓점 제거
    while (draft_.size() >= 2) {
        const QPointF a = draft_[draft_.size() - 1];
        const QPointF b = draft_[draft_.size() - 2];
        if (qAbs(a.x() - b.x()) < kDupEps && qAbs(a.y() - b.y()) < kDupEps)
            draft_.removeLast();
        else
            break;
    }
    if (draft_.size() >= 3) {
        roi_ = draft_;
        emit roiCompleted(channel_, roi_);
    }
    setDrawMode(false);  // draft_ 비우고 커서 복원
}
