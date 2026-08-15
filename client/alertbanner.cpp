#include "alertbanner.h"

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

    setActiveAlerts({});   // 생성 직후 0건 상태를 렌더해 둔다
}

void AlertBanner::setActiveAlerts(const QList<AlertItem>& items)
{
    // 1. 표시할 줄 텍스트 목록과 배너 전체 등급을 만든다.
    QStringList lineTexts;
    QString overallSeverity;

    if (items.isEmpty()) {
        // 0건 고정 카피(PD-07) — 도형까지 포함한 리터럴 문자열이며 계산 결과가
        // 아니다.
        lineTexts << QStringLiteral("✓ 활성 경보 없음 · 모든 침대 정상 범위");
        overallSeverity = QStringLiteral("normal");
    } else {
        int bestRank = -1;
        for (const AlertItem& item : items) {
            lineTexts << item.glyph + QStringLiteral(" ") + item.eventLabel +
                              QStringLiteral(" · ") + item.location;
            const int rank = severityRank(item.severity);
            if (rank > bestRank) {
                bestRank = rank;
                overallSeverity = item.severity;
            }
        }
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
