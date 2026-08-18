#ifndef ACTIVITYCHART_H
#define ACTIVITYCHART_H

#include <QVector>
#include <QWidget>

// 하루 24시간 활동량 그래프 (일일 리포트용).
//
// 걸음 수는 "그 시간 동안의 합계"라 막대로, 심박은 "그때그때의 값"이라 꺾은선으로
// 그린다. 합계를 선으로 이으면 없는 중간값이 있는 것처럼 보이고, 순간값을 막대로
// 그리면 0부터 쌓인 양처럼 보인다 — 둘을 바꾸면 그래프가 거짓말을 한다.
//
// Sparkline 과 같이 신호/슬롯이 없어 Q_OBJECT 가 필요 없다.
// 색은 theme.h 의 전역 팔레트를 매 페인트마다 읽으므로 라이트/다크 전환을 따라간다.
class ActivityChart : public QWidget {
public:
    explicit ActivityChart(QWidget* parent = nullptr);

    // 24시간치 값을 한 번에 넣는다. 두 벡터 모두 길이 24 여야 하며,
    // 모자라면 뒤를 0 으로 채우고 넘치면 자른다.
    //   stepsPerHour : 그 시간의 걸음 합계 (0 이면 안 움직인 시간)
    //   hrPerHour    : 그 시간의 평균 심박, 0 = 측정값 없음(선을 끊는다)
    void setData(const QVector<int>& stepsPerHour, const QVector<int>& hrPerHour);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<int> steps_;   // 24개
    QVector<int> hr_;      // 24개, 0 = 값 없음
    bool hasData_ = false;
};

#endif  // ACTIVITYCHART_H
