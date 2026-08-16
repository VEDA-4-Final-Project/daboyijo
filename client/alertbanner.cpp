#include "alertbanner.h"

#include <algorithm>

#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// 등급 문자열 → 순위 정수. 다건 상태에서 배너 전체 severity(=최고 등급)를
// 고르는 데만 쓴다. 센서 수치를 비교하지 않는다 — 이것은 다섯 개 문자열
// 사이의 ISA 에스컬레이션 표시 규칙이지 임계값 판정이 아니다(PD-06).
int severityRank(const QString& severity)
{
    if (severity == QStringLiteral("critical")) return 4;
    if (severity == QStringLiteral("high"))     return 3;
    if (severity == QStringLiteral("medium"))   return 2;
    if (severity == QStringLiteral("info"))     return 1;
    return 0;  // normal 및 알 수 없는 값
}

// 배너가 동시에 그릴 수 있는 최대 줄 수(04-UI-SPEC §3.4). 4건 이상이면
// 상위 2건 + 요약 줄(+N건 더)로 이 값에 맞춘다. 5줄 상한값이 아니라 이
// 상수 하나만 낮추면 §5.2의 1차 완화책(3→2)이 그대로 적용된다.
constexpr int kMaxVisibleLines = 3;

}  // namespace

AlertBanner::AlertBanner(QWidget* parent) : QFrame(parent)
{
    // 이 한 줄이 base.qss:110-119의 48px 헤드라인 + 심각도별 배경 규칙을
    // 즉시 끌어온다 — 신규 QSS를 쓰지 않는다.
    setObjectName("alertBanner");

    lineLayout_ = new QVBoxLayout(this);
    // 패딩은 #alertBanner QSS 규칙의 padding: 24px 32px가 이미 준다 —
    // 여백을 두 번 주지 않는다.
    lineLayout_->setContentsMargins(0, 0, 0, 0);
    lineLayout_->setSpacing(8);   // Phase 2 간격 스케일 sm

    setActiveAlerts({});   // 생성 직후 0건 상태이므로 숨겨 둔다
}

void AlertBanner::setActiveAlerts(const QList<AlertItem>& items)
{
    if (items.isEmpty()) {
        // 0건이면 자리 자체를 접는다 — "활성 경보 없음" 고정 카피 대신
        // 위젯을 숨겨 dashboardOuter 레이아웃에서 공간을 비운다.
        setVisible(false);
        return;
    }
    setVisible(true);

    // 1. 표시할 줄 텍스트 목록과 배너 전체 등급을 만든다.
    QStringList lineTexts;
    QString overallSeverity;

    // ① 배경색을 정하는 overallSeverity는 상한 적용 전 전체 items에서
    // 고른다 — 접힌 항목의 등급이 배경색에 반영되지 않는 실패를 막는다.
    int bestRank = -1;
    for (const AlertItem& item : items) {
        const int rank = severityRank(item.severity);
        if (rank > bestRank) {
            bestRank = rank;
            overallSeverity = item.severity;
        }
    }

    // ② 등급 내림차순 안정 정렬 — 동순위 항목은 collectAlertItems()가
    // 넣은 채널 오름차순·이벤트 순서를 그대로 유지한다. 순서를 보장하지
    // 않는 정렬 함수는 쓰지 않는다 — 반드시 stable 계열이어야 한다.
    QList<AlertItem> sorted = items;
    std::stable_sort(sorted.begin(), sorted.end(),
                      [](const AlertItem& a, const AlertItem& b) {
                          return severityRank(a.severity) > severityRank(b.severity);
                      });

    // ③④ kMaxVisibleLines 이하면 전부 줄로, 넘으면 상위 2건 + 요약 줄.
    // 상한은 항목 개수 정수 비교로만 건다 — 문자열을 자르거나
    // 말줄임하지 않는다(한글 음절이 중간에서 깨지는 실패 모드 차단).
    if (sorted.size() <= kMaxVisibleLines) {
        for (const AlertItem& item : sorted) {
            lineTexts << item.glyph + QStringLiteral(" ") + item.eventLabel +
                              QStringLiteral(" · ") + item.location;
        }
    } else {
        for (int i = 0; i < 2; ++i) {
            const AlertItem& item = sorted[i];
            lineTexts << item.glyph + QStringLiteral(" ") + item.eventLabel +
                              QStringLiteral(" · ") + item.location;
        }
        lineTexts << QStringLiteral("+%1건 더").arg(sorted.size() - 2);
    }

    // 2. 줄 라벨 개수를 맞춘다 — 부족하면 만들고, 남으면 숨긴다(삭제하지 않는다).
    while (lines_.size() < lineTexts.size()) {
        auto* label = new QLabel();
        // objectName을 부여하지 않는다 — 글자색은 부모(#alertBanner)의 color
        // 상속에 맡긴다(UI-SPEC §4.3). 자체 색 규칙을 걸지 않는다.
        lineLayout_->addWidget(label);
        lines_.append(label);
    }
    for (int i = lineTexts.size(); i < lines_.size(); ++i) {
        lines_[i]->hide();
    }

    // 3. 사용하는 줄에 텍스트를 세팅하고 보인다.
    for (int i = 0; i < lineTexts.size(); ++i) {
        lines_[i]->setText(lineTexts[i]);
        lines_[i]->show();
    }

    // 4. 프레임 자신에게 severity 속성을 걸고 4단계 관용구로 repolish한다.
    setProperty("severity", overallSeverity);
    style()->unpolish(this);
    style()->polish(this);
    update();
}
