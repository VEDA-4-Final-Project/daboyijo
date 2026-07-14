#ifndef VIDEOVIEW_H
#define VIDEOVIEW_H

#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
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

    void setFrame(const QPixmap& frame);      // 새 영상 프레임 표시
    void setRoi(const QPolygonF& normPts);    // 확정된 ROI(정규화) 주입
    QPolygonF roi() const { return roi_; }
    void clearRoi();

    void setDrawMode(bool on);                // 그리기 시작/중단
    bool drawMode() const { return drawMode_; }
    void cancelDraft();                       // 그리던 것 버리고 그리기 종료
    void setRoiVisible(bool on);              // 모니터링 오버레이 표시 토글

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
};

#endif  // VIDEOVIEW_H
