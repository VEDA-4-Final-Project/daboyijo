#ifndef SPARKLINE_H
#define SPARKLINE_H

#include <QColor>
#include <QVector>
#include <QWidget>

// 최근 값들을 작은 선그래프로 그리는 미니 스파크라인 (심박 추세용).
// 신호/슬롯이 없어 Q_OBJECT 불필요. addValue로 값을 밀어넣으면 오래된 건 밀려난다.
class Sparkline : public QWidget {
public:
    explicit Sparkline(QWidget* parent = nullptr);

    void addValue(double v);              // 새 값 추가 (capacity 초과분은 폐기)
    void setLineColor(const QColor& c);   // 선·채움 색 (상태색)
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<double> values_;
    int capacity_ = 40;                    // 보관할 최근 점 개수
    QColor color_{0x17, 0xC7, 0xB6};       // 기본 청록
};

#endif  // SPARKLINE_H
