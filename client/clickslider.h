#ifndef CLICKSLIDER_H
#define CLICKSLIDER_H

#include <QSlider>

// 값 숫자를 우측에 직접 그려주고, 트랙 아무 곳이나 클릭하면 그 위치로 핸들이
// 점프하는 슬라이더. (Busbom 카메라 설정의 ClickableSlider를 다보이조 테마에
// 맞춰 이식) 새 시그널/슬롯이 없어 Q_OBJECT 불필요 — QSlider의 valueChanged
// 등 기존 시그널은 그대로 쓸 수 있다. (sparkline.h와 같은 무-Q_OBJECT 위젯)
//
// 값 숫자를 그리는 우측 여백(kValueMargin)만큼 트랙을 좁혀 쓴다 — 클릭 비율
// 계산도 이 좁힌 폭 기준이라 숫자 영역을 눌러도 최대값으로 튀지 않는다.
class ClickSlider : public QSlider {
public:
    explicit ClickSlider(QWidget* parent = nullptr);

    static constexpr int kValueMargin = 40;  // 우측 값 숫자용 여백(px)

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
};

#endif  // CLICKSLIDER_H
