#ifndef VITALTILE_H
#define VITALTILE_H

#include <QColor>
#include <QFrame>
#include <QString>

class QLabel;
class Sparkline;

// 바이탈 카드 1장 — 이름·병상·SpO2·심박·상태 배지·심박 추세를 그리는 표시 전용
// 위젯. 신호/슬롯이 없어 Q_OBJECT를 생략했다.
//   ⚠ sparkline.h·clickslider.h는 Q_OBJECT를 쓴다 — 이 클래스가 예외다.
//   Q_OBJECT 없이도 되는 이유는 (1) 시그널/슬롯이 없고 (2) qobject_cast 대상이
//   아니며 (3) QSS가 objectName 셀렉터(#severityDot 등)로만 이 위젯을 잡기
//   때문이다. 동적 속성 셀렉터([severity="..."])는 QObject 기능이라 moc 없이도
//   동작한다. 셋 중 하나라도 깨지면 — 특히 base.qss에 `VitalTile { ... }` 같은
//   타입 셀렉터를 추가하면 metaObject()->className()이 "QFrame"을 돌려줘
//   규칙이 조용히 안 먹는다 — 그때 Q_OBJECT를 추가할 것.
// 임계값 판정·등급 도형·등급 색 계산은 여기 없다 — MainWindow가 계산해
// setLive()/setStale()로 완성된 표시값을 주입한다(D-02).
class VitalTile : public QFrame {
public:
    explicit VitalTile(QWidget* parent = nullptr);

    // 이름·병상 표기 갱신. 멱등 — 둘 다 이전 값과 같으면 즉시 반환한다.
    // (같은 입소자가 다른 채널로 재배정돼 병상만 바뀐 경우도 호출부가
    // 조건 없이 불러도 안전하도록)
    void setIdentity(const QString& name, const QString& bedText);

    // 신호가 살아 있을 때. severity/badgeText는 MainWindow가 이미 판정한
    // 완성값이며, 이 함수는 그 값을 표시만 한다.
    void setLive(int spo2, int heartRate, const QString& severity,
                 const QString& badgeText, const QColor& sparkColor);

    // 무신호(대기·신호 끊김·미착용) — 세 상태의 구분은 badgeText 문자열로만
    // 표현된다. severity는 이 타일 안에서 판정하지 않는다.
    void setStale(const QString& badgeText, const QColor& sparkColor);

    // 심박 샘플 1개를 추세 그래프에 위임. 이력 저장소(hrHistory_)는
    // MainWindow가 위젯 밖에 따로 들고 있다 — 이 함수는 그래프 점만 찍는다.
    void pushHeartRateSample(double bpm);

private:
    QLabel* dot_ = nullptr;
    QLabel* nameLbl_ = nullptr;
    QLabel* bedLbl_ = nullptr;
    QLabel* badgeLbl_ = nullptr;
    QLabel* spo2Value_ = nullptr;
    QLabel* hrValue_ = nullptr;
    Sparkline* spark_ = nullptr;

    // setIdentity() 멱등 가드용 보관값.
    QString name_;
    QString bedText_;
};

#endif  // VITALTILE_H
