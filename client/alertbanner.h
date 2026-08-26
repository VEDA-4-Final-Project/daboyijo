#ifndef ALERTBANNER_H
#define ALERTBANNER_H

#include <QList>
#include <QString>
#include <QFrame>

class QLabel;
class QVBoxLayout;

// 활성 경보 한 건 — MainWindow::collectAlertItems()가 채워 AlertBanner에
// 그대로 주입한다. 등급 판정(critical/high/medium/info/normal)과 도형 선택은
// 전부 MainWindow가 끝내고, 이 구조체는 완성된 표시값만 담는다(D-02 승계).
struct AlertItem {
    int channel = -1;      // 0-based
    QString glyph;         // 등급 도형(✖▲◆●✓○) — MainWindow가 severityGlyph()로 채운다
    QString eventLabel;    // "낙상" / "침상이탈" / "생체신호 이상"
    QString location;      // patients[channel].bed — 채널 단위 표기(PD-05)
    QString severity;      // "critical" / "high" / "medium" / "info" / "normal"
};

// Phase 4(ALERT-03)가 메인 화면에 상시 노출할 경보 배너 — 표시 전용 위젯.
// 신호/슬롯이 없어 moc 매크로(vitaltile.h와 동일 관용구)를 생략했다.
//   ⚠ sparkline.h·clickslider.h는 그 매크로를 쓴다 — 이 클래스가 예외다.
//   생략이 안전한 이유는 (1) 시그널/슬롯이 없고 (2) qobject_cast 대상이
//   아니며 (3) QSS가 objectName 셀렉터(#alertBanner)로만 이 위젯을 잡기
//   때문이다. 동적 속성 셀렉터([severity="..."])는 QObject 기능이라 moc 없이도
//   동작한다. 셋 중 하나라도 깨지면 — 특히 base.qss에 타입 셀렉터를 추가하면
//   metaObject()->className()이 "QFrame"을 돌려줘 규칙이 조용히 안 먹는다 —
//   그때 이 매크로를 추가할 것.
// 등급 판정·도형 선택은 여기 없다 — MainWindow가 계산해 AlertItem으로
// 완성된 표시값을 주입한다(D-02). 이 위젯은 버튼도 신호도 갖지 않는다 —
// 기존 슬라이드 토스트(#alarmToast)가 이미 경보 해제 진입점을 갖고 있다.
class AlertBanner : public QFrame {
public:
    explicit AlertBanner(QWidget* parent = nullptr);

    // 활성 경보 목록을 통째로 받아 다시 그린다. 비어 있으면 위젯을 숨긴다 —
    // "활성 경보 없음" 고정 카피는 쓰지 않는다.
    void setActiveAlerts(const QList<AlertItem>& items);

private:
    QVBoxLayout* lineLayout_ = nullptr;
    QList<QLabel*> lines_;   // 재사용 가능한 줄 라벨 풀 — 다건↔0건 전환 시 만들고 버리지 않는다
};

#endif  // ALERTBANNER_H
