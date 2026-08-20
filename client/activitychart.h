#ifndef ACTIVITYCHART_H
#define ACTIVITYCHART_H

#include <QVector>
#include <QWidget>

// 하루 24시간 활동량(걸음 수) 그래프 — 일일 리포트용.
//
// 걸음 수는 "그 시간 동안의 합계"라 막대로 그린다. 선으로 이으면 점과 점 사이에
// 없는 중간값이 있는 것처럼 보이는데, 합계에는 그런 값이 없다.
//
// Sparkline 과 같이 신호/슬롯이 없어 Q_OBJECT 가 필요 없다.
// 색은 theme.h 의 전역 팔레트를 매 페인트마다 읽으므로 라이트/다크 전환을 따라간다.
class ActivityChart : public QWidget {
public:
    explicit ActivityChart(QWidget* parent = nullptr);

    // 24시간치 걸음 합계를 한 번에 넣는다. 길이가 모자라면 뒤를 0 으로 채우고
    // 넘치면 자른다. 0 인 시간은 막대를 그리지 않는다(자거나 안 움직인 시간).
    void setData(const QVector<int>& stepsPerHour);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<int> steps_;   // 24개
    bool hasData_ = false;
};

#endif  // ACTIVITYCHART_H
