#ifndef TIMELINEBAR_H
#define TIMELINEBAR_H

#include <QColor>
#include <QVector>
#include <QWidget>

// 관제 화면 하단 타임라인 — Wisenet Viewer 하단 바에 대응하는 위젯.
//
// 세 층을 한 줄에 겹쳐 그린다:
//   ① 시각 눈금(HH:mm) — 창 길이에 맞춰 5분/15분/1시간… 간격을 자동으로 고른다.
//   ② 녹화 구간 막대 — NVR 세그먼트가 실제로 존재하는 시간대만 초록으로 채운다.
//      "녹화가 없는 구간"이 눈에 보여야 관제사가 헛되이 그 시각을 찾지 않는다.
//   ③ 이벤트 마커 — 낙상·침상이탈이 난 시각에 세로 눈금. 클릭 목표점이 된다.
// 그 위에 플레이헤드(선택 강조색)와 "지금" 선을 얹는다.
//
// 좌표계는 전부 Unix epoch 밀리초다. 위젯은 시간→x 변환만 알고 재생은 모른다 —
// 클릭/드래그는 seekRequested(ms)로 내보내고 실제 탐색은 호출부가 한다.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    // 녹화 구간 1개 [startMs, endMs). 세그먼트가 연속이면 호출부가 미리 합쳐 준다.
    struct Span {
        qint64 startMs = 0;
        qint64 endMs = 0;
    };
    // 이벤트 마커 1개. color 로 종류(낙상/침상이탈/생체이상)를 구분한다.
    struct Marker {
        qint64 atMs = 0;
        QColor color;
    };

    explicit TimelineBar(QWidget* parent = nullptr);

    // 표시할 시간 창. endMs <= startMs 면 무시한다.
    void setWindow(qint64 startMs, qint64 endMs);
    qint64 windowStart() const { return startMs_; }
    qint64 windowEnd() const { return endMs_; }
    // 창을 "지금"에 맞춰 오른쪽 끝으로 민다(라이브 추종). 길이는 그대로.
    void followNow();

    void setSpans(const QVector<Span>& spans);
    void setMarkers(const QVector<Marker>& markers);

    // 플레이헤드 위치. 창 밖이면 그리지 않는다(-1 = 없음).
    void setPlayhead(qint64 ms);
    qint64 playhead() const { return playheadMs_; }

    // 라이브 모드에선 플레이헤드 대신 "지금" 선만 강조한다.
    void setLiveMode(bool on);

signals:
    // 사용자가 타임라인의 특정 시각을 찍었다(클릭/드래그). 재생 위치 요청.
    void seekRequested(qint64 ms);
    // 드래그가 끝난 시점(마우스 뗌) — 연속 탐색이 부담스러운 호출부용.
    void seekCommitted(qint64 ms);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    qreal xForMs(qint64 ms) const;    // 시각 → 위젯 x
    qint64 msForX(qreal x) const;     // 위젯 x → 시각(창 밖은 잘라낸다)
    qint64 tickStepMs() const;        // 현재 창 길이에 어울리는 눈금 간격
    QRectF trackRect() const;         // 녹화 막대가 그려지는 띠

    qint64 startMs_ = 0;
    qint64 endMs_ = 0;
    qint64 playheadMs_ = -1;
    bool liveMode_ = true;
    bool dragging_ = false;
    qreal hoverX_ = -1;               // 커서 위치(툴팁 눈금용), 음수면 없음

    QVector<Span> spans_;
    QVector<Marker> markers_;
};

#endif  // TIMELINEBAR_H
