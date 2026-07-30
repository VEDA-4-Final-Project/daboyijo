#ifndef SPARKLINE_H
#define SPARKLINE_H

#include <QColor>
#include <QPair>
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
    // Y축을 고정 범위로 (미지정이면 최근값 min~max에 자동 스케일).
    void setRange(double lo, double hi);
    // 수평 기준선(점선) 목록 — {값, 색}. 예: 주의/위험 임계선.
    void setGuides(const QVector<QPair<double, QColor>>& guides);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<double> values_;
    int capacity_ = 40;                    // 보관할 최근 점 개수
    QColor color_{0x17, 0xC7, 0xB6};       // 기본 청록
    bool fixedRange_ = false;              // 고정 Y 범위 사용 여부
    double rangeLo_ = 0.0, rangeHi_ = 1.0;
    QVector<QPair<double, QColor>> guides_;  // 기준선(점선)
};

#endif  // SPARKLINE_H
