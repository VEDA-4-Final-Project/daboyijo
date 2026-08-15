#include "vitaltile.h"

#include "sparkline.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

VitalTile::VitalTile(QWidget* parent) : QFrame(parent)
{
    setObjectName("vitalCard");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── 헤더 바: 상태등 + 이름 + 병상 + 상태 배지 ──
    auto* head = new QFrame();
    head->setObjectName("vitalHead");
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(10, 2, 9, 2);   // v2: 위아래 7→2
    hl->setSpacing(6);
    dot_ = new QLabel();
    // #severityDot(24px)이 아니라 #vitalDot(9px)로 되돌린다.
    // 24px는 "몇 미터 밖에서 보이는" 경보 스케일이고, 이 타일은 가까이서
    // 읽는 요소다 — 알림 스케일이 데스크 요소로 새어 나온 사례였다(IA-03).
    // 실제 증상: 24px 점이 헤더 행 높이를 밀어올려 옆의 상태 배지가 세로로
    // 늘어났다.
    dot_->setObjectName("vitalDot");
    nameLbl_ = new QLabel();
    nameLbl_->setObjectName("vitalName");
    bedLbl_ = new QLabel();
    bedLbl_->setObjectName("vitalBed");
    badgeLbl_ = new QLabel(QStringLiteral("대기"));
    badgeLbl_->setObjectName("vitalBadge");
    badgeLbl_->setAlignment(Qt::AlignCenter);
    hl->addWidget(dot_);
    hl->addWidget(nameLbl_);
    hl->addWidget(bedLbl_);
    hl->addStretch();
    // 세로 가운데 정렬 — 명시하지 않으면 배지가 행 높이만큼 늘어난다.
    hl->addWidget(badgeLbl_, 0, Qt::AlignVCenter);
    lay->addWidget(head);

    // ── 본문: 큰 판독값 2개 (산소포화도 / 심박) — 환자 모니터 느낌 ──
    auto* body = new QHBoxLayout();
    body->setContentsMargins(12, 7, 12, 5);
    body->setSpacing(8);

    auto makeStat = [&](const QString& icon, const QString& caption,
                         const QString& unit, QLabel*& valueRef) {
        auto* box = new QFrame();
        box->setObjectName("statBox");
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(11, 5, 11, 5);
        bl->setSpacing(2);
        auto* cap = new QLabel(icon + QStringLiteral("  ") + caption);
        cap->setObjectName("statCaption");
        valueRef = new QLabel(QStringLiteral("--"));
        valueRef->setObjectName("statValue");
        auto* unitLbl = new QLabel(unit);
        unitLbl->setObjectName("statUnit");
        auto* valRow = new QHBoxLayout();
        valRow->setContentsMargins(0, 0, 0, 0);
        valRow->setSpacing(4);
        valRow->addWidget(valueRef);
        valRow->addWidget(unitLbl, 0, Qt::AlignBottom);
        valRow->addStretch();
        bl->addWidget(cap);
        bl->addLayout(valRow);
        return box;
    };

    body->addWidget(makeStat(QStringLiteral("🫁"), QStringLiteral("산소포화도"),
                              QStringLiteral("%"), spo2Value_));
    body->addWidget(makeStat(QStringLiteral("❤"), QStringLiteral("심박"),
                              QStringLiteral("bpm"), hrValue_));
    lay->addLayout(body);

    // ── 심박 미니 추세 그래프 (고정 스케일 40~140 + 주의/위험 점선) ──
    auto* sparkRow = new QHBoxLayout();
    sparkRow->setContentsMargins(12, 0, 12, 7);
    spark_ = new Sparkline();
    spark_->setRange(40, 140);
    spark_->setGuides({
        {110.0, QColor(QString::fromLatin1(kCritical))},  // 고 위험
        {100.0, QColor(QString::fromLatin1(kWarn))},      // 고 주의
        { 55.0, QColor(QString::fromLatin1(kWarn))},      // 저 주의
        { 45.0, QColor(QString::fromLatin1(kCritical))},  // 저 위험
    });
    sparkRow->addWidget(spark_);
    lay->addLayout(sparkRow);
}

void VitalTile::setIdentity(const QString& name, const QString& bedText)
{
    // 멱등 가드(PD-01) — 호출부가 매번 조건 없이 불러도, 변화가 없으면
    // 여기서 즉시 반환한다. 비교를 호출부에 맡기면 필드 하나를 빠뜨렸을 때
    // 조용히 낡는다.
    if (name_ == name && bedText_ == bedText) return;
    name_ = name;
    bedText_ = bedText;
    nameLbl_->setText(name_);
    bedLbl_->setText(bedText_);
}

void VitalTile::setLive(int spo2, int heartRate, const QString& severity,
                         const QString& badgeText, const QColor& sparkColor)
{
    // 센서가 못 읽어 0이 오면 숫자 대신 "--"(PD-03 — 임계값 등급 판정이
    // 아니라 센서 무값 표기 규칙이라 타일 안에 둔다).
    spo2Value_->setText(spo2 > 0 ? QString::number(spo2) : QStringLiteral("--"));
    spo2Value_->setProperty("severity", severity);
    spo2Value_->setProperty("vital", "live");
    spo2Value_->style()->unpolish(spo2Value_);
    spo2Value_->style()->polish(spo2Value_);
    spo2Value_->update();

    hrValue_->setText(QString::number(heartRate));
    hrValue_->setProperty("severity", severity);
    hrValue_->setProperty("vital", "live");
    hrValue_->style()->unpolish(hrValue_);
    hrValue_->style()->polish(hrValue_);
    hrValue_->update();

    dot_->setProperty("severity", severity);
    dot_->setProperty("vital", "live");
    dot_->style()->unpolish(dot_);
    dot_->style()->polish(dot_);
    dot_->update();

    // 배지 도형은 호출부가 이미 접두해 넘긴다 — 타일은 고르지 않는다(D-02).
    badgeLbl_->setText(badgeText);
    badgeLbl_->setProperty("severity", severity);
    badgeLbl_->setProperty("vital", "live");
    badgeLbl_->style()->unpolish(badgeLbl_);
    badgeLbl_->style()->polish(badgeLbl_);
    badgeLbl_->update();

    spark_->setLineColor(sparkColor);
}

void VitalTile::setStale(const QString& badgeText, const QColor& sparkColor)
{
    // 대기/신호 끊김/미착용 세 상태의 구분은 호출부가 만든 badgeText로만
    // 표현된다 — 이 타일은 어느 상태인지 판정하지 않는다.
    spo2Value_->setText(QStringLiteral("--"));
    spo2Value_->setProperty("severity", "");
    spo2Value_->setProperty("vital", "stale");
    spo2Value_->style()->unpolish(spo2Value_);
    spo2Value_->style()->polish(spo2Value_);
    spo2Value_->update();

    hrValue_->setText(QStringLiteral("--"));
    hrValue_->setProperty("severity", "");
    hrValue_->setProperty("vital", "stale");
    hrValue_->style()->unpolish(hrValue_);
    hrValue_->style()->polish(hrValue_);
    hrValue_->update();

    dot_->setProperty("severity", "");
    dot_->setProperty("vital", "stale");
    dot_->style()->unpolish(dot_);
    dot_->style()->polish(dot_);
    dot_->update();

    badgeLbl_->setText(badgeText);
    badgeLbl_->setProperty("severity", "");
    badgeLbl_->setProperty("vital", "stale");
    badgeLbl_->style()->unpolish(badgeLbl_);
    badgeLbl_->style()->polish(badgeLbl_);
    badgeLbl_->update();

    spark_->setLineColor(sparkColor);
}

void VitalTile::pushHeartRateSample(double bpm)
{
    spark_->addValue(bpm);
}
