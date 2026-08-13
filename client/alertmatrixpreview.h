#ifndef ALERTMATRIXPREVIEW_H
#define ALERTMATRIXPREVIEW_H

#include <QColor>
#include <QString>
#include <QWidget>

class QTimer;

// 알림 노드의 64x32 HUB75 패널을 관제 앱 안에서 그대로 흉내 내는 미리보기.
//
// alert-node/app/matrix/alert-display.cpp 의 렌더링(같은 hub75-font16 글꼴, 오른쪽→왼쪽
// 스크롤, 위아래 등급색 테두리)을 Qt 로 이식해 WYSIWYG 를 맞춘다. 밝기 슬라이더를 올리면
// 픽셀 색이 곧바로 그 레벨(brightness/255)로 밝아진다 — 이게 이 위젯의 핵심 목적이다.
//
// 실제 패널과 다른 점(의도한 단순화): 긴급(CRIT) 테두리 깜빡임은 넣지 않는다. 밝기를
// 눈으로 가늠하는 설정 화면이라 깜빡임이 오히려 방해된다.
class AlertMatrixPreview : public QWidget
{
    Q_OBJECT
public:
    explicit AlertMatrixPreview(QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(300, 128); }

public slots:
    // 0~255. 알림 노드 setBrightness 와 같은 범위. 즉시 다시 그린다.
    void setBrightness(int v255);
    // 스크롤할 문구. 글꼴에 없는 글자는 8px 공백으로 대체(원본과 동일).
    void setText(const QString& text);
    // 전체 밝기 기준 등급색(테두리·글자). 기본은 낙상 경보 빨강.
    void setAccentColor(const QColor& c);

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;   // 보일 때만 스크롤(안 보이면 타이머 정지)
    void hideEvent(QHideEvent* e) override;

private slots:
    void advance();

private:
    int      measureText(const QString& s) const;   // 픽셀 폭(글꼴 adv 합)

    QTimer*  timer_ = nullptr;
    QString  text_;
    QColor   accent_{255, 30, 30};   // sevColor(CRIT) 와 동일
    int      brightness_ = 180;      // 0~255
    int      offset_ = 0;            // 스크롤 위치(px)
    int      span_ = 0;              // 한 바퀴 길이 = measureText + 64
};

#endif  // ALERTMATRIXPREVIEW_H
