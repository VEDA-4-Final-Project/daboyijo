#ifndef VIDEOVIEW_H
#define VIDEOVIEW_H

#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QTimer>
#include <QWidget>

// 영상 1채널을 표시하고, 그 위에 침대 ROI를 그리거나 보여주는 위젯.
//
//  - 영상은 비율 유지(KeepAspectRatio)로 위젯 안에 레터박스 표시된다.
//  - ROI 좌표는 "실제 영상이 그려진 사각형" 기준 0~1 정규화로 저장한다.
//    이 좌표계는 서버의 Detection.cx/cy(0~1)와 동일 → 서버가 받아 그대로 판정.
//    레터박스 여백을 빼고 정규화하므로 위젯 크기가 바뀌어도 좌표가 안 틀어진다.
//  - 그리기 모드: 좌클릭=꼭짓점 추가, 더블클릭/우클릭=완료, cancelDraft()=취소.
class VideoView : public QWidget {
    Q_OBJECT
public:
    explicit VideoView(int channel, QWidget* parent = nullptr);

    int channel() const { return channel_; }
    // 표시 채널 변경 (ROI 편집기처럼 한 위젯을 여러 채널에 재사용할 때).
    // 그리던 것/미리보기는 초기화하고, roi_·frame_는 호출부가 새로 넣는다.
    void setChannel(int ch);

    void setFrame(const QPixmap& frame);      // 새 영상 프레임 표시
    // 카메라 연결 여부. false면 "카메라 미연결" 안내를 표시하고 이전 프레임을 지운다.
    // (연결 요청 직후~첫 프레임 도착 전에는 true로 두면 "신호 대기 중"이 표시된다.)
    void setCameraConnected(bool on);
    bool cameraConnected() const { return cameraConnected_; }

    // NVR 스타일 오버레이 — 영상 위에 직접 그리는 정보.
    void setOverlayInfo(const QString& info);  // 좌하단 라벨(예: "201호-1 · 전승현")
    void setLive(bool on);                      // 우상단 LIVE/미연결 표시등
    bool live() const { return live_; }
    // 영상 모서리를 둥글게(0=직각). 카드(#videoCard/#camStage)의 radius와 맞춰
    // 모서리에 검은 각이 삐져나오지 않게 한다.
    void setCornerRadius(int r);
    // 우상단 웨어러블 바이탈 오버레이 (체온·심박). color는 상태색(정상/주의/위험).
    void setVitals(const QString& temp, const QString& hr, const QColor& color);
    void setRoi(const QPolygonF& normPts);    // 확정된 ROI(정규화) 주입
    QPolygonF roi() const { return roi_; }
    void clearRoi();

    void setDrawMode(bool on);                // 그리기 시작/중단
    bool drawMode() const { return drawMode_; }
    void cancelDraft();                       // 그리던 것 버리고 그리기 종료
    void setRoiVisible(bool on);              // 모니터링 오버레이 표시 토글
    // 낙상/침상이탈 경보 — 빨간 테두리 점멸 + 상단 배너(label).
    // normPt(0~1) 주면 발생 위치에 마커 표시.
    void setAlert(bool on, const QString& label = QStringLiteral("🚨 낙상 감지"),
                  const QPointF& normPt = QPointF(-1, -1));

signals:
    void roiCompleted(int channel, const QPolygonF& normPts);  // 그리기 완료
    void drawModeChanged(int channel, bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    QRectF displayRect() const;                    // 레터박스 제외 실제 영상 사각형
    QPointF toNorm(const QPointF& widgetPt) const; // 위젯 픽셀 → 정규화 0~1
    QPointF toWidget(const QPointF& normPt) const; // 정규화 0~1 → 위젯 픽셀
    void finishDraft();                            // draft_ → roi_ 확정 + 시그널

    int channel_;
    QPixmap frame_;
    QPolygonF roi_;            // 확정된 ROI (정규화)
    QPolygonF draft_;          // 그리는 중인 점들 (정규화)
    QPointF hover_;            // 현재 커서 위치 (정규화, 미리보기 선용)
    bool hasHover_ = false;
    bool drawMode_ = false;
    bool roiVisible_ = true;
    bool cameraConnected_ = false;  // 카메라 연결 상태 (시작 시 미연결)
    int cornerRadius_ = 0;          // 영상 모서리 반경(0=직각)
    QString overlayInfo_;           // 좌하단 오버레이 라벨(병상·이름)
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
