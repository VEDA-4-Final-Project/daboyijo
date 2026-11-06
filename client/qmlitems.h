#ifndef QMLITEMS_H
#define QMLITEMS_H

// ═══════════════════════════════════════════════════════════
//  QML 화면용 그리기 아이템 — 위젯 앱의 커스텀 위젯을 그대로 옮긴 것
// ═══════════════════════════════════════════════════════════
// 위젯 앱의 VideoView · TimelineBar · Sparkline · AlarmOverlay는 QPainter로 직접
// 그린다. QML에서도 같은 모양이 나오도록 같은 그리기 코드를 QQuickPaintedItem으로
// 옮겼다(색은 theme.h의 같은 전역 토큰을 읽는다). 편집 기능(ROI 그리기 등)은 빼고
// 관제 화면이 보여주는 부분만 담았다.

#include <QColor>
#include <QImage>
#include <QQuickImageProvider>
#include <QVariantList>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVector>

class FrameProvider;

// ── 영상 타일 (VideoView 관제 표시 부분) ─────────────────────
class VideoItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY changed)
    Q_PROPERTY(int frameId READ frameId WRITE setFrameId NOTIFY changed)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY changed)
    Q_PROPERTY(bool alert READ alert WRITE setAlert NOTIFY changed)
    Q_PROPERTY(QString alertLabel READ alertLabel WRITE setAlertLabel NOTIFY changed)
    Q_PROPERTY(qreal alertNx READ alertNx WRITE setAlertNx NOTIFY changed)
    Q_PROPERTY(qreal alertNy READ alertNy WRITE setAlertNy NOTIFY changed)
    // 침대 ROI: [{id, label, pts:[x0,y0,x1,y1,...]}] (0~1 정규화) — 위젯 VideoView::setZones와 같은 입력
    Q_PROPERTY(QVariantList zones READ zones WRITE setZones NOTIFY changed)
    // ROI 편집(장치 설정): 그리기 모드·선택된 침대 — 위젯 VideoView 의 drawMode/selectedZone
    Q_PROPERTY(bool drawMode READ drawMode WRITE setDrawMode NOTIFY drawModeChanged)
    Q_PROPERTY(int selectedZone READ selectedZone WRITE setSelectedZone NOTIFY selectedZoneChanged)
    Q_PROPERTY(bool editable READ editable WRITE setEditable NOTIFY changed)

public:
    explicit VideoItem(QQuickItem* parent = nullptr);
    static void setFrameSource(FrameProvider* f) { source_ = f; }

    int channel() const { return channel_; }
    void setChannel(int c) { channel_ = c; reload(); }
    int frameId() const { return frameId_; }
    void setFrameId(int id) { if (id == frameId_) return; frameId_ = id; reload(); }
    bool live() const { return live_; }
    void setLive(bool v) { if (v == live_) return; live_ = v; update(); emit changed(); }
    QString displayName() const { return displayName_; }
    void setDisplayName(const QString& n) { displayName_ = n; update(); emit changed(); }
    bool alert() const { return alert_; }
    void setAlert(bool on);
    QString alertLabel() const { return alertLabel_; }
    void setAlertLabel(const QString& t) { alertLabel_ = t; update(); emit changed(); }
    qreal alertNx() const { return alertNx_; }
    void setAlertNx(qreal v) { alertNx_ = v; update(); emit changed(); }
    qreal alertNy() const { return alertNy_; }
    void setAlertNy(qreal v) { alertNy_ = v; update(); emit changed(); }

    QVariantList zones() const { return zones_; }
    void setZones(const QVariantList& z) { zones_ = z; update(); emit changed(); }

    bool drawMode() const { return drawMode_; }
    void setDrawMode(bool on);
    int selectedZone() const { return selectedZone_; }
    void setSelectedZone(int id) { if (id == selectedZone_) return; selectedZone_ = id; update(); emit selectedZoneChanged(); }
    bool editable() const { return editable_; }
    void setEditable(bool on) { editable_ = on; setAcceptedMouseButtons(on ? (Qt::LeftButton | Qt::RightButton) : Qt::NoButton); setAcceptHoverEvents(on); emit changed(); }
    Q_INVOKABLE int nextFreeZoneId() const;
    Q_INVOKABLE void cancelDraft() { setDrawMode(false); }

    void paint(QPainter* p) override;

signals:
    void changed();
    void drawModeChanged();
    void selectedZoneChanged();
    void roiCompleted(int roiId, const QVariantList& pts);   // 0~1 정규화 [x0,y0,x1,y1,...]
    void zoneClicked(int roiId);                              // -1 = 빈 곳

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void hoverMoveEvent(QHoverEvent* e) override;

private:
    void reload();
    void finishDraft();
    QPointF toNorm(const QPointF& w) const;
    int zoneAt(const QPointF& n) const;
    QRectF displayRect() const;
    QPointF toItem(const QPointF& n) const;

    static FrameProvider* source_;
    int channel_ = 0;
    int frameId_ = 0;
    bool live_ = false;
    QString displayName_;
    QImage frame_;
    bool alert_ = false;
    bool alertBlink_ = false;
    QString alertLabel_;
    qreal alertNx_ = -1, alertNy_ = -1;
    QTimer alertTimer_;
    QVariantList zones_;
    bool drawMode_ = false, editable_ = false, hasHover_ = false;
    int selectedZone_ = -1;
    QVector<QPointF> draft_;
    QPointF hover_;
};

// ── 타임라인 (TimelineBar: 눈금 · 녹화 구간 · 이벤트 마커 · 플레이헤드) ──
class TimelineItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool liveMode READ liveMode WRITE setLiveMode NOTIFY changed)
    Q_PROPERTY(double playhead READ playhead WRITE setPlayhead NOTIFY changed)   // epoch ms, -1 = 없음
public:
    explicit TimelineItem(QQuickItem* parent = nullptr);
    bool liveMode() const { return liveMode_; }
    void setLiveMode(bool on) { liveMode_ = on; update(); emit changed(); }
    double playhead() const { return double(playheadMs_); }
    void setPlayhead(double ms) { playheadMs_ = qint64(ms); update(); emit changed(); }
    Q_INVOKABLE void addMarker(qint64 atMs, const QColor& color);
    // 녹화 구간 [[startMs,endMs],...] / 마커 [{atMs,color}] — 위젯 TimelineBar::setSpans·setMarkers
    Q_INVOKABLE void setSpans(const QVariantList& spans);
    Q_INVOKABLE void setMarkers(const QVariantList& markers);
    Q_INVOKABLE void followNow();
    void paint(QPainter* p) override;
signals:
    void changed();
    void seekRequested(double ms);     // 클릭·드래그 중
    void seekCommitted(double ms);     // 마우스를 뗀 순간
protected:
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void hoverMoveEvent(QHoverEvent* e) override;
    void hoverLeaveEvent(QHoverEvent* e) override;
private:
    qreal xForMs(qint64 ms) const;
    qint64 msForX(qreal x) const;
    qint64 tickStepMs() const;
    qint64 startMs_ = 0, endMs_ = 0, playheadMs_ = -1;
    bool liveMode_ = true, dragging_ = false;
    qreal hoverX_ = -1;
    struct Span { qint64 startMs, endMs; };
    QVector<Span> spans_;
    struct Marker { qint64 atMs; QColor color; };
    QVector<Marker> markers_;
    QTimer followTimer_;
};

// ── 심박 스파크라인 (Sparkline, VitalTile 설정 그대로) ──────────
class SparklineItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor NOTIFY changed)
    Q_PROPERTY(QVariantList values READ values WRITE setValues NOTIFY changed)
public:
    explicit SparklineItem(QQuickItem* parent = nullptr);
    QColor lineColor() const { return color_; }
    void setLineColor(const QColor& c) { color_ = c; update(); emit changed(); }
    Q_INVOKABLE void addValue(double v);
    QVariantList values() const;
    void setValues(const QVariantList& v);
    void paint(QPainter* p) override;
signals:
    void changed();
private:
    QVector<double> values_;
    QColor color_;
};

// ── 경보 비네트 (AlarmOverlay) ────────────────────────────────
class AlarmOverlayItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY changed)
public:
    explicit AlarmOverlayItem(QQuickItem* parent = nullptr);
    bool active() const { return active_; }
    void setActive(bool on);
    void paint(QPainter* p) override;
signals:
    void changed();
private:
    bool active_ = false;
    double phase_ = 0.0;
    QTimer timer_;
};

// ── 로그인 배경 (LoginDialog::makePlaceholder: 브랜드색 블롭 + 어두운 베일) ──
class BackdropItem : public QQuickPaintedItem
{
    Q_OBJECT
public:
    explicit BackdropItem(QQuickItem* parent = nullptr) : QQuickPaintedItem(parent) {}
    void paint(QPainter* p) override;
};

// ── 시간별 활동량 막대 (ActivityChart) ────────────────────────
class ActivityChartItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QVariantList steps READ steps WRITE setSteps NOTIFY changed)
public:
    explicit ActivityChartItem(QQuickItem* parent = nullptr) : QQuickPaintedItem(parent) {}
    QVariantList steps() const;
    void setSteps(const QVariantList& v);
    void paint(QPainter* p) override;
signals:
    void changed();
private:
    QVector<int> steps_;
    bool hasData_ = false;
};

// ── 아이콘 (uiicons.h 선 아이콘) ─────────────────────────────
// image://icons/nav/<종류>/<색>   image://icons/tool/<종류>/<색>
class IconProvider : public QQuickImageProvider
{
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}
    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override;
};

#endif // QMLITEMS_H
