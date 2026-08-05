#include "videoview.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
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

// ── 스켈레톤 오버레이 ──────────────────────────────────────────
const QColor kSkelLine(0, 200, 255);          // 관절 연결선(시안 — ROI 초록/경보 빨강과 구분)
const QColor kSkelJoint(255, 255, 255);       // 관절 점(흰색)
constexpr float kSkelMinScore = 0.20f;        // 이 신뢰도 미만 관절은 안 그림
// 자세 추론은 객체당 ~2fps라, 이 시간 넘게 갱신 없으면 스켈레톤을 지운다(잔상 방지).
constexpr qint64 kPoseExpireMs = 2500;
// MoveNet(COCO) 17관절 연결(뼈대). 인덱스는 pose_estimator.hpp의 관절 순서와 동일:
// 0코 1좌눈 2우눈 3좌귀 4우귀 5좌어깨 6우어깨 7좌팔꿈 8우팔꿈 9좌손목 10우손목
// 11좌엉덩 12우엉덩 13좌무릎 14우무릎 15좌발목 16우발목
const int kSkelEdges[][2] = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4},            // 얼굴
    {5, 6},                                    // 어깨
    {5, 7}, {7, 9}, {6, 8}, {8, 10},           // 팔
    {5, 11}, {6, 12}, {11, 12},                // 몸통
    {11, 13}, {13, 15}, {12, 14}, {14, 16},    // 다리
};
}  // namespace

VideoView::VideoView(int channel, QWidget* parent)
    : QWidget(parent), channel_(channel) {
    setMouseTracking(true);  // 그리기 미리보기 선을 위해 이동 이벤트 항상 수신
    setMinimumHeight(160);
    setAttribute(Qt::WA_OpaquePaintEvent);  // 매 프레임 전체를 다시 그림
}

void VideoView::setFrame(const QPixmap& frame) {
    frame_ = frame;
    cameraConnected_ = true;  // 프레임이 들어왔다는 건 카메라가 연결됐다는 뜻
    update();
}

void VideoView::setChannel(int ch) {
    channel_ = ch;
    draft_.clear();
    hasHover_ = false;
    drawMode_ = false;
    poses_.clear();  // 이전 채널 스켈레톤 잔상 제거
    setCursor(Qt::ArrowCursor);
    update();  // roi_·frame_는 호출부(선택 슬롯)가 새 채널 값으로 채운다
}

void VideoView::setCameraConnected(bool on) {
    if (cameraConnected_ == on) return;
    cameraConnected_ = on;
    if (!on) frame_ = QPixmap();  // 미연결이면 이전 프레임 제거(정지화면 오해 방지)
    update();
}

void VideoView::setOverlayInfo(const QString& info) {
    overlayInfo_ = info;
    update();
}

void VideoView::setLive(bool on) {
    if (live_ == on) return;
    live_ = on;
    update();
}

void VideoView::setVitals(const QString& temp, const QString& hr, const QColor& color) {
    vitalTemp_ = temp;
    vitalHr_ = hr;
    vitalColor_ = color;
    hasVitals_ = true;
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

void VideoView::setPose(int objectId, const QVector<QPointF>& normPts,
                        const QVector<float>& scores) {
    PoseSkeleton& s = poses_[objectId];
    s.pts = normPts;
    s.scores = scores;
    s.updatedMs = QDateTime::currentMSecsSinceEpoch();
    update();
}

void VideoView::clearPoses() {
    if (poses_.isEmpty()) return;
    poses_.clear();
    update();
}

void VideoView::setPoseVisible(bool on) {
    if (poseVisible_ == on) return;
    poseVisible_ = on;
    update();
}

void VideoView::setAlert(bool on, const QString& label, const QPointF& normPt) {
    alertPt_ = normPt;
    if (on) alertLabel_ = label;   // 켤 때의 문구를 점멸 내내 유지
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
    // 비율 유지하되 칸을 꽉 채우고(cover) 넘치는 부분은 중앙 크롭 → 검은 레터박스 제거.
    // displayRect는 여전히 "프레임 전체"의 배치 사각형이므로, 여기에 대한 0~1 정규화는
    // 서버 Detection.cx/cy(0~1, 프레임 전체 기준)와 그대로 일치한다.
    fs.scale(size(), Qt::KeepAspectRatioByExpanding);
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

    if (!cameraConnected_) {
        p.setPen(QColor(139, 148, 158));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("📷 카메라 미연결\n상단 '카메라 연결'을 눌러 CCTV를 연결하세요"));
    } else if (frame_.isNull()) {
        p.setPen(QColor(139, 148, 158));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("신호 대기 중…"));
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

    // 스켈레톤(자세) 오버레이 — ROI 위, 경보 마커 아래
    if (poseVisible_ && !frame_.isNull() && !poses_.isEmpty()) drawPoses(p);

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

    // ── NVR 스타일 오버레이 (영상 위 정보: 채널·이름 / LIVE) ──
    {
        const int m = 8;

        // 좌하단: CH 태그(청록) + 병상·이름(회백), 반투명 검정 캡슐 위에.
        QFont lf = font();
        lf.setBold(true);
        p.setFont(lf);
        const QFontMetrics fm(lf);
        const QString chTag = QStringLiteral("CH%1").arg(channel_ + 1);
        const QString info = overlayInfo_;
        const int gap = info.isEmpty() ? 0 : 8;
        const int chW = fm.horizontalAdvance(chTag);
        const int infoW = fm.horizontalAdvance(info);
        const int boxH = fm.height() + 8;
        const int boxW = 10 + chW + gap + infoW + 10;
        QRectF box(m, height() - m - boxH, boxW, boxH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 145));
        p.drawRoundedRect(box, 5, 5);
        qreal tx = box.left() + 10;
        p.setPen(QColor(0x17, 0xC7, 0xB6));
        p.drawText(QRectF(tx, box.top(), chW + 2, box.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, chTag);
        if (!info.isEmpty()) {
            tx += chW + gap;
            p.setPen(QColor(0xE6, 0xED, 0xF3));
            p.drawText(QRectF(tx, box.top(), infoW + 2, box.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, info);
        }

        // 우상단: LIVE(빨강 점) / 미연결(회색 점) 표시등.
        QFont sf = font();
        sf.setBold(true);
        p.setFont(sf);
        const QFontMetrics sfm(sf);
        const QString st = live_ ? QStringLiteral("LIVE") : QStringLiteral("미연결");
        const QColor dotc = live_ ? QColor(0xFF, 0x5A, 0x5F) : QColor(0x8B, 0x98, 0xA5);
        const qreal dr = 6;
        const int stW = sfm.horizontalAdvance(st);
        const int pillH = sfm.height() + 6;
        const int pillW = 8 + int(dr) + 6 + stW + 8;
        QRectF pill(width() - m - pillW, m, pillW, pillH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 145));
        p.drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
        p.setBrush(dotc);
        p.drawEllipse(QPointF(pill.left() + 8 + dr / 2, pill.center().y()), dr / 2, dr / 2);
        p.setPen(QColor(0xE6, 0xED, 0xF3));
        p.drawText(QRectF(pill.left() + 8 + dr + 6, pill.top(), stW + 4, pill.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, st);
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

            const qreal bw = p.fontMetrics().horizontalAdvance(alertLabel_) + 28;
            p.setBrush(red);
            p.setPen(Qt::NoPen);
            QRectF badge((width() - bw) / 2.0, 8, bw, 24);
            p.drawRoundedRect(badge, 6, 6);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, alertLabel_);
        }
    }
}

// 스켈레톤 렌더 — 만료된 건 지우고, 신뢰도 높은 관절/뼈대만 그린다.
void VideoView::drawPoses(QPainter& p) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (auto it = poses_.begin(); it != poses_.end();) {
        PoseSkeleton& s = it.value();
        if (now - s.updatedMs > kPoseExpireMs) {
            it = poses_.erase(it);  // 오래 갱신 안 된 스켈레톤 제거(잔상 방지)
            continue;
        }

        const int n = s.pts.size();
        auto ok = [&](int i) {
            return i < n && i < s.scores.size() && s.scores[i] >= kSkelMinScore;
        };

        // 뼈대(연결선) — 양 끝 관절이 모두 신뢰될 때만
        p.setPen(QPen(kSkelLine, 2));
        for (const auto& e : kSkelEdges) {
            if (ok(e[0]) && ok(e[1]))
                p.drawLine(toWidget(s.pts[e[0]]), toWidget(s.pts[e[1]]));
        }

        // 관절 점
        p.setPen(Qt::NoPen);
        p.setBrush(kSkelJoint);
        for (int i = 0; i < n; ++i) {
            if (ok(i)) p.drawEllipse(toWidget(s.pts[i]), 3, 3);
        }

        ++it;
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
