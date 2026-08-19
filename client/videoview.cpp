#include "videoview.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "theme.h"

namespace {
// 침대 ROI 오버레이 색. 한 화면에 침대가 여럿이라 하나의 색으로는 구분이 안 돼
// 번호별로 고정 배정한다 — 색이 매번 바뀌면 관제사가 "초록이 1번"으로 못 외운다.
// 첫 색은 기존 단일 ROI와 같은 브랜드 강조색(힐링 그린)을 유지.
const QColor kZoneColors[] = {
    QColor(47, 158, 143),   // 힐링 그린
    QColor(93, 143, 214),   // 블루
    QColor(214, 149, 61),   // 앰버
    QColor(178, 122, 201),  // 퍼플
    QColor(96, 176, 96),    // 그린
    QColor(214, 106, 122),  // 로즈
    QColor(122, 176, 194),  // 틸
    QColor(184, 160, 96),   // 카키
};
const QColor kDraftLine(224, 148, 43);        // 그리는 중(주황)
const QColor kVertex(245, 247, 245);          // 꼭짓점 마커
constexpr double kDupEps = 0.006;             // 더블클릭 중복 꼭짓점 제거 임계
constexpr int kMaxPoints = 32;                // 프로토콜 DBJ_ROI_MAX_POINTS와 동일
}  // namespace

QColor VideoView::zoneColor(int id) {
    const int n = int(sizeof(kZoneColors) / sizeof(kZoneColors[0]));
    return kZoneColors[(id < 0 ? 0 : id) % n];
}

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
    selectedZone_ = -1;
    setCursor(Qt::ArrowCursor);
    update();  // zones_·frame_는 호출부(선택 슬롯)가 새 채널 값으로 채운다
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

void VideoView::setDisplayName(const QString& name) {
    if (displayName_ == name) return;
    displayName_ = name;
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

void VideoView::setCornerRadius(int r) {
    if (cornerRadius_ == r) return;
    cornerRadius_ = r;
    // 둥근 모서리는 바깥 네 귀퉁이를 안 칠하므로 부모 배경이 비쳐야 한다
    // → 그때만 '위젯 전체를 내가 칠한다'는 최적화 속성을 끈다.
    setAttribute(Qt::WA_OpaquePaintEvent, r <= 0);
    update();
}

void VideoView::setZones(const QVector<RoiZone>& zones) {
    zones_ = zones;
    // 선택돼 있던 침대가 목록에서 사라졌으면 선택도 같이 푼다
    if (selectedZone_ >= 0) {
        bool found = false;
        for (const auto& z : zones_) {
            if (z.id == selectedZone_) { found = true; break; }
        }
        if (!found) selectedZone_ = -1;
    }
    update();
}

void VideoView::clearZones() {
    zones_.clear();
    selectedZone_ = -1;
    update();
}

void VideoView::removeZone(int id) {
    for (int i = 0; i < zones_.size(); ++i) {
        if (zones_[i].id != id) continue;
        zones_.removeAt(i);
        break;
    }
    if (selectedZone_ == id) selectedZone_ = -1;
    update();
}

int VideoView::nextFreeZoneId() const {
    for (int id = 0; id < kMaxRoiZones; ++id) {
        bool used = false;
        for (const auto& z : zones_) {
            if (z.id == id) { used = true; break; }
        }
        if (!used) return id;
    }
    return -1;  // 침대가 상한까지 찼다
}

void VideoView::setSelectedZone(int id) {
    if (selectedZone_ == id) return;
    selectedZone_ = id;
    update();
}

int VideoView::zoneAt(const QPointF& n) const {
    // 겹쳐 그린 침대는 나중에 그린 쪽이 위에 보이므로, 보이는 것과 집히는 것을
    // 맞추려면 역순으로 훑는다.
    for (int i = zones_.size() - 1; i >= 0; --i) {
        if (zones_[i].poly.size() >= 3 && zones_[i].poly.containsPoint(n, Qt::OddEvenFill))
            return zones_[i].id;
    }
    return -1;
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
    // kVideoSurface는 Palette 구조체 밖 독립 상수라 팔레트 전환 함수(applyPalette)가
    // 닿을 통로 자체가 없다(D-07/IA-02). 이 색이 밝아지면 아래에서 그리는 반투명
    // 오버레이(이름표 캡슐·CH태그·LIVE 배지)들의 가독성이 전부 깨지므로, 둥근 모서리
    // 분기와 사각형 분기가 반드시 같은 QColor 인스턴스를 공유해야 한다.
    const QColor videoSurface(QString::fromLatin1(kVideoSurface));
    if (cornerRadius_ > 0) {
        // 카드 radius에 맞춰 영상·오버레이 전부를 둥글게 잘라낸다.
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), cornerRadius_, cornerRadius_);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillPath(clip, videoSurface);
        p.setClipPath(clip);
    } else {
        p.fillRect(rect(), videoSurface);
    }

    if (!cameraConnected_) {
        p.setPen(QColor(139, 148, 158));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("📷 카메라 미연결\nCCTV를 연결하세요"));
    } else if (frame_.isNull()) {
        p.setPen(QColor(139, 148, 158));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("신호 대기 중…"));
    } else {
        p.drawPixmap(displayRect(), frame_, frame_.rect());
    }

    p.setRenderHint(QPainter::Antialiasing, true);

    // 확정된 침대 ROI들 (모니터링 오버레이)
    if (roiVisible_) {
        QFont zf = font();
        zf.setBold(true);
        zf.setPointSizeF(qMax(7.5, zf.pointSizeF() - 1));
        const QFontMetrics zfm(zf);

        for (const auto& z : zones_) {
            if (z.poly.size() < 2) continue;
            const bool sel = (z.id == selectedZone_);
            const QColor line = zoneColor(z.id);
            QColor fill = line;
            fill.setAlpha(sel ? 105 : 70);   // 선택된 침대는 조금 더 진하게

            QPolygonF poly;
            for (const auto& n : z.poly) poly << toWidget(n);
            QPainterPath path;
            path.addPolygon(poly);
            path.closeSubpath();
            p.fillPath(path, fill);
            p.setPen(QPen(line, sel ? 3 : 2));
            p.drawPolygon(poly);

            // 침대 이름표 — 다각형 중심에 얹는다. 침대가 여럿이면 어느 게
            // 누구 자리인지 영상 위에서 바로 읽혀야 관제가 된다.
            const QString text = z.label.isEmpty()
                                     ? QStringLiteral("침대 %1").arg(z.id + 1)
                                     : z.label;
            const QPointF c = poly.boundingRect().center();
            const int tw = zfm.horizontalAdvance(text);
            const int th = zfm.height();
            QRectF chip(c.x() - tw / 2.0 - 7, c.y() - th / 2.0 - 3, tw + 14, th + 6);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 165));
            p.drawRoundedRect(chip, 5, 5);
            p.setFont(zf);
            p.setPen(line.lighter(135));
            p.drawText(chip, Qt::AlignCenter, text);
        }
        p.setFont(font());
        p.setBrush(Qt::NoBrush);
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

    // ── 타일 오버레이 (Wisenet Viewer 배치: 좌상단 [LIVE] 카메라 이름) ──
    // 예전엔 CH태그가 좌하단, LIVE가 우상단으로 흩어져 있었다. 4분할에서 눈이
    // 두 모서리를 오가야 해서 "몇 번 채널이 살아있나"를 한 번에 못 읽는다.
    // 한 줄로 붙이면 왼쪽 위만 훑어도 채널·상태·사람이 동시에 들어온다.
    // (우상단은 타일 닫기 버튼 자리라 비워 둔다 — mainwindow가 그 위에 얹는다.)
    {
        const int m = 8;
        QFont lf = font();
        lf.setBold(true);
        p.setFont(lf);
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
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 155));
        p.drawRoundedRect(box, 4, 4);

        // 상태 점 + LIVE
        p.setBrush(dotc);
        p.drawEllipse(QPointF(box.left() + 8 + dr / 2, box.center().y()), dr / 2, dr / 2);
        qreal tx = box.left() + 8 + dr + 6;
        p.setPen(live_ ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x8B, 0x98, 0xA5));
        p.drawText(QRectF(tx, box.top(), stW + 2, box.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, st);
        // 카메라 이름 — 항상 밝은 회백으로, LIVE보다 한 톤 낮게.
        tx += stW + 10;
        p.setPen(QColor(0xE6, 0xED, 0xF3));
        p.drawText(QRectF(tx, box.top(), nameW + 2, box.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, name);

        // 좌하단 보조 라벨(병상·이름 등) — 지정됐을 때만.
        if (!overlayInfo_.isEmpty()) {
            const int iw = fm.horizontalAdvance(overlayInfo_);
            QRectF ib(m, height() - m - boxH, iw + 20, boxH);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 145));
            p.drawRoundedRect(ib, 4, 4);
            p.setPen(QColor(0xE6, 0xED, 0xF3));
            p.drawText(ib.adjusted(10, 0, -10, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, overlayInfo_);
        }
    }

    // 그리기 모드 안내 배너 — 지금 몇 번 침대를 그리는 중인지 같이 보여준다.
    // 좌상단 이름표(LIVE·채널명) 아래에 놓는다: 예전엔 y=6에 그려서 이름표와
    // 글자가 겹쳐 둘 다 못 읽었다. 배경 캡슐도 깔아 밝은 영상 위에서도 읽히게 한다.
    if (drawMode_) {
        const int id = nextFreeZoneId();
        const QString msg =
            id < 0 ? QStringLiteral("침대가 가득 찼습니다 (최대 %1개)").arg(kMaxRoiZones)
                   : QStringLiteral("침대 %1 영역 클릭 · 더블클릭(또는 우클릭)으로 완료").arg(id + 1);

        QFont bf = font();
        bf.setBold(true);
        p.setFont(bf);
        const QFontMetrics bfm(bf);
        const int tw = bfm.horizontalAdvance(msg);
        const int bh = bfm.height() + 8;
        // 이름표(높이 = fm.height()+6, 위 여백 8)보다 아래로 내린다.
        const int top = 8 + bfm.height() + 6 + 6;
        QRectF banner((width() - tw - 24) / 2.0, top, tw + 24, bh);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 175));
        p.drawRoundedRect(banner, 4, 4);
        p.setPen(kDraftLine);
        p.drawText(banner, Qt::AlignCenter, msg);
        p.setFont(font());
        p.setBrush(Qt::NoBrush);
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

void VideoView::mousePressEvent(QMouseEvent* e) {
    // 그리기 중이든 아니든, 왼쪽 클릭은 언제나 "이 타일을 고른다"를 겸한다.
    // 침대를 안 눌러도 타일 선택은 일어나야 관제 그리드에서 대상 채널이 바뀐다.
    if (e->button() == Qt::LeftButton) emit tileClicked(channel_);

    if (!drawMode_) {
        // 그리기 중이 아니면 클릭은 "이 침대를 편집 대상으로 고른다"는 뜻.
        // 빈 곳을 누르면 선택 해제(-1).
        if (e->button() == Qt::LeftButton && !zones_.isEmpty()) {
            const int hit = zoneAt(toNorm(e->position()));
            setSelectedZone(hit);
            emit zoneSelected(channel_, hit);
        }
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
    const int id = nextFreeZoneId();
    if (draft_.size() >= 3 && id >= 0) {
        RoiZone z;
        z.id = id;
        z.poly = draft_;
        zones_.push_back(z);       // 이름표는 호출부가 입소자를 지정하며 채운다
        selectedZone_ = id;        // 방금 그린 침대를 바로 편집 대상으로
        emit roiCompleted(channel_, id, z.poly);
    }
    setDrawMode(false);  // draft_ 비우고 커서 복원
}

// ── FramePreview (이미지 탭 적용 전/후 프리뷰) ────────────────
FramePreview::FramePreview(const QString& placeholder, QWidget* parent)
    : QWidget(parent), placeholder_(placeholder) {}

void FramePreview::setFrame(const QPixmap& frame) {
    frame_ = frame;
    update();  // 다시 그리기만 예약 — 레이아웃은 건드리지 않는다
}

void FramePreview::clearFrame() {
    if (frame_.isNull()) return;
    frame_ = QPixmap();
    update();
}

// 비율 유지(KeepAspectRatio) 레터박스 배치. 클릭 지점 초점 좌표 계산도 이 값을 쓴다.
void FramePreview::setCaption(const QString& text, bool live) {
    caption_ = text;
    captionLive_ = live;
    update();
}

qreal FramePreview::frameAspect() const {
    if (frame_.isNull() || frame_.height() <= 0) return 16.0 / 9.0;
    return qreal(frame_.width()) / qreal(frame_.height());
}

QRectF FramePreview::imageRect() const {
    if (frame_.isNull()) return QRectF(rect());
    QSizeF fs = frame_.size();
    fs.scale(size(), Qt::KeepAspectRatio);
    return QRectF((width() - fs.width()) / 2.0, (height() - fs.height()) / 2.0,
                  fs.width(), fs.height());
}

void FramePreview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath frameShape;
    frameShape.addRoundedRect(box, 8, 8);
    // kVideoSurface — VideoView::paintEvent()와 같은 단일 출처(D-07/IA-02).
    p.fillPath(frameShape, QColor(QString::fromLatin1(kVideoSurface)));

    if (frame_.isNull()) {
        p.setPen(QColor(QString::fromLatin1(kTextSub)));
        p.drawText(rect(), Qt::AlignCenter, placeholder_);
    } else {
        p.save();
        p.setClipPath(frameShape);
        // 스케일은 QPainter에 맡긴다 — 원본을 매번 CPU로 리샘플링하지 않는다.
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(imageRect(), frame_, QRectF(frame_.rect()));
        p.restore();
    }

    // 좌상단 이름표 — 영상 위에 얹는다(관제 타일의 LIVE 배지와 같은 방식).
    if (!caption_.isEmpty()) {
        QFont cf = font();
        cf.setBold(true);
        cf.setPixelSize(13);
        p.setFont(cf);
        const QFontMetrics fm(cf);

        const qreal m = 8, dr = 7;
        const int tw = fm.horizontalAdvance(caption_);
        const int h = fm.height() + 8;
        const int dotW = captionLive_ ? int(dr) + 7 : 0;
        const int w = 10 + dotW + tw + 11;
        QRectF chip(m, m, w, h);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 165));
        p.drawRoundedRect(chip, 4, 4);

        qreal tx = chip.left() + 10;
        if (captionLive_) {
            p.setBrush(QColor(0xFF, 0x5A, 0x5F));
            p.drawEllipse(QPointF(tx + dr / 2, chip.center().y()), dr / 2, dr / 2);
            tx += dr + 7;
        }
        p.setPen(QColor(0xF2, 0xF6, 0xFA));
        p.drawText(QRectF(tx, chip.top(), tw + 2, chip.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, caption_);
    }

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(QString::fromLatin1(kBorder)), 1));
    p.drawRoundedRect(box, 8, 8);
}
