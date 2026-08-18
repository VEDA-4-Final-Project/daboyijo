#ifndef VIDEOVIEW_H
#define VIDEOVIEW_H

#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

// 침대 1개 = ROI 1개. 한 채널(=한 병실 시야)에 침대가 여럿이고 침대마다 입소자가
// 다르므로, ROI는 채널당 1개가 아니라 id로 구분되는 목록이다.
// id는 서버 프로토콜의 roi_id와 같은 값이며 채널 안에서만 유효하다.
struct RoiZone {
    int id = 0;          // 채널 안 침대 번호 (0 ~ kMaxRoiZones-1)
    QPolygonF poly;      // 정규화 0~1 다각형
    QString label;       // 화면에 띄울 이름 (예: "침대 1 · 김복순")
};

// 침대 ROI 상한 — 서버 프로토콜 DBJ_ROI_MAX_ZONES와 같은 값으로 유지할 것
static constexpr int kMaxRoiZones = 8;

// 영상 1채널을 표시하고, 그 위에 침대 ROI들을 그리거나 보여주는 위젯.
//
//  - 영상은 비율 유지(KeepAspectRatio)로 위젯 안에 레터박스 표시된다.
//  - ROI 좌표는 "실제 영상이 그려진 사각형" 기준 0~1 정규화로 저장한다.
//    이 좌표계는 서버의 Detection.cx/cy(0~1)와 동일 → 서버가 받아 그대로 판정.
//    레터박스 여백을 빼고 정규화하므로 위젯 크기가 바뀌어도 좌표가 안 틀어진다.
//  - 그리기 모드: 좌클릭=꼭짓점 추가, 더블클릭/우클릭=완료, cancelDraft()=취소.
//    완료하면 비어 있는 가장 작은 id를 새 침대에 부여한다.
//  - 그리기 모드가 아닐 때 침대를 클릭하면 선택된다(zoneSelected) — 인스펙터에서
//    그 침대에 입소자를 지정하거나 삭제할 대상을 고르는 용도.
class VideoView : public QWidget {
    Q_OBJECT
public:
    explicit VideoView(int channel, QWidget* parent = nullptr);

    int channel() const { return channel_; }
    // 표시 채널 변경 (ROI 편집기처럼 한 위젯을 여러 채널에 재사용할 때).
    // 그리던 것/선택/미리보기는 초기화하고, zones_·frame_는 호출부가 새로 넣는다.
    void setChannel(int ch);

    void setFrame(const QPixmap& frame);      // 새 영상 프레임 표시
    // 카메라 연결 여부. false면 "카메라 미연결" 안내를 표시하고 이전 프레임을 지운다.
    // (연결 요청 직후~첫 프레임 도착 전에는 true로 두면 "신호 대기 중"이 표시된다.)
    void setCameraConnected(bool on);
    bool cameraConnected() const { return cameraConnected_; }

    // NVR 스타일 오버레이 — 영상 위에 직접 그리는 정보.
    // 배치는 Wisenet Viewer를 따른다: 좌상단에 [LIVE] + 카메라 이름 한 줄.
    void setOverlayInfo(const QString& info);  // 좌하단 라벨(예: "201호-1 · 전승현")
    // 좌상단 LIVE 배지 옆에 붙는 카메라 이름("CH1 · 김복순"). 비우면 채널 태그만.
    void setDisplayName(const QString& name);
    void setLive(bool on);                      // 좌상단 LIVE/미연결 표시등
    bool live() const { return live_; }
    // 영상 모서리를 둥글게(0=직각). 카드(#videoCard/#camStage)의 radius와 맞춰
    // 모서리에 검은 각이 삐져나오지 않게 한다.
    void setCornerRadius(int r);
    // 우상단 웨어러블 바이탈 오버레이 (체온·심박). color는 상태색(정상/주의/위험).
    void setVitals(const QString& temp, const QString& hr, const QColor& color);
    // ── 침대 ROI (채널당 여러 개) ──────────────────────────────
    void setZones(const QVector<RoiZone>& zones);  // 목록 통째로 주입
    const QVector<RoiZone>& zones() const { return zones_; }
    bool hasZones() const { return !zones_.isEmpty(); }
    void clearZones();                        // 전부 제거
    void removeZone(int id);                  // 침대 1개 제거
    // 비어 있는 가장 작은 침대 번호. 다 찼으면 -1.
    int nextFreeZoneId() const;

    int selectedZone() const { return selectedZone_; }
    void setSelectedZone(int id);             // -1이면 선택 해제

    // 침대 번호별 오버레이 색 — 번호만 보고도 어느 침대인지 구분되게 고정 배정.
    // 인스펙터의 침대 목록 배지도 같은 색을 써야 영상과 목록이 눈으로 짝지어진다.
    static QColor zoneColor(int id);

    void setDrawMode(bool on);                // 그리기 시작/중단
    bool drawMode() const { return drawMode_; }
    void cancelDraft();                       // 그리던 것 버리고 그리기 종료
    void setRoiVisible(bool on);              // 모니터링 오버레이 표시 토글
    // 낙상/침상이탈 경보 — 빨간 테두리 점멸 + 상단 배너(label).
    // normPt(0~1) 주면 발생 위치에 마커 표시.
    void setAlert(bool on, const QString& label = QStringLiteral("🚨 낙상 감지"),
                  const QPointF& normPt = QPointF(-1, -1));

signals:
    // 그리기 완료 — roiId는 이 위젯이 고른 빈 번호. 호출부가 서버로 전송한다.
    void roiCompleted(int channel, int roiId, const QPolygonF& normPts);
    void drawModeChanged(int channel, bool on);
    // 그리기 모드가 아닐 때 침대를 클릭 — roiId=-1이면 빈 곳을 눌러 선택 해제
    void zoneSelected(int channel, int roiId);
    // 타일 아무 곳이나 눌렀다 — 관제 그리드에서 "지금 다루는 채널"을 바꾸는 신호.
    // zoneSelected와 별개다: 침대를 안 눌러도(빈 영상 위여도) 타일 선택은 일어난다.
    void tileClicked(int channel);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    QRectF displayRect() const;                    // 레터박스 제외 실제 영상 사각형
    QPointF toNorm(const QPointF& widgetPt) const; // 위젯 픽셀 → 정규화 0~1
    QPointF toWidget(const QPointF& normPt) const; // 정규화 0~1 → 위젯 픽셀
    void finishDraft();                            // draft_ → zones_ 확정 + 시그널
    // 클릭 지점(정규화)이 어느 침대 안인지. 없으면 -1.
    // 겹쳐 그린 침대는 나중에 그린 쪽(뒤 원소)이 위에 보이므로 역순으로 찾는다.
    int zoneAt(const QPointF& normPt) const;

    int channel_;
    QPixmap frame_;
    QVector<RoiZone> zones_;   // 확정된 침대 ROI들 (정규화)
    int selectedZone_ = -1;    // 인스펙터가 편집 중인 침대 (-1 = 없음)
    QPolygonF draft_;          // 그리는 중인 점들 (정규화)
    QPointF hover_;            // 현재 커서 위치 (정규화, 미리보기 선용)
    bool hasHover_ = false;
    bool drawMode_ = false;
    bool roiVisible_ = true;
    bool cameraConnected_ = false;  // 카메라 연결 상태 (시작 시 미연결)
    int cornerRadius_ = 0;          // 영상 모서리 반경(0=직각)
    QString overlayInfo_;           // 좌하단 오버레이 라벨(병상·이름)
    QString displayName_;           // 좌상단 LIVE 옆 카메라 이름
    bool live_ = false;             // 우상단 LIVE 표시등 상태
    QString vitalTemp_, vitalHr_;   // 우상단 바이탈(체온·심박) 텍스트
    QColor vitalColor_{0x8B, 0x98, 0xA5};  // 바이탈 상태색(기본 회색)
    bool hasVitals_ = false;        // 바이탈 값 수신 여부

    bool alert_ = false;          // 낙상/침상이탈 경보 상태
    bool alertBlink_ = false;     // 점멸 on/off
    QString alertLabel_ = QStringLiteral("🚨 낙상 감지");  // 상단 배너 문구
    QPointF alertPt_{-1, -1};     // 발생 위치(정규화 0~1), 음수면 미표시
    QTimer* alertTimer_ = nullptr;
};

// 이미지 탭의 '적용 전 / 적용 후' 비교 프리뷰 패널.
//
// QLabel + setPixmap(원본.scaled(...))을 쓰면 안 된다:
//   · setPixmap이 sizeHint를 바꿔 매 프레임 전체 레이아웃이 무효화되고,
//   · QPixmap::scaled(SmoothTransformation)는 원본 해상도를 GUI 스레드에서
//     통째로 리샘플링한다.
// 둘이 겹치면 GUI 스레드가 영상 수신 속도를 못 따라가 지연이 눈에 띄게 쌓인다.
// 그래서 VideoView와 같이 원본을 그대로 들고 있다가 paintEvent에서 한 번
// 그리기만 한다(레이아웃 무효화 없음).
class FramePreview : public QWidget {
public:
    explicit FramePreview(const QString& placeholder, QWidget* parent = nullptr);

    void setFrame(const QPixmap& frame);   // 새 프레임(원본 해상도 그대로 보관)
    void clearFrame();                     // 비우고 안내 문구로 되돌림
    bool hasFrame() const { return !frame_.isNull(); }
    QRectF imageRect() const;              // 레터박스 여백 뺀 실제 영상 사각형

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPixmap frame_;
    QString placeholder_;
};

#endif  // VIDEOVIEW_H
