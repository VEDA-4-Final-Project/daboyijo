#include "vitaltile.h"

#include "sparkline.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// 판독값 캡션 아이콘 — 이모지(🫁 ❤) 대신 직접 그리는 선 아이콘.
// 이모지를 쓰면 (1) OS마다 컬러 이모지 폰트가 제각각이라 톤이 깨지고
// (2) 관제 소프트웨어에 컬러 이모지가 있으면 즉시 아마추어로 읽힌다.
// helpButton(mainwindow.cpp:878~)이 이미 쓰는 방식과 같다 — 2배 DPR로
// 그려 고해상도에서도 또렷하다.
//
// kind: 0 = SpO2(산소 방울), 1 = HR(심전도 파형)
QPixmap makeVitalIcon(int kind, const QColor& c)
{
    const int d = 14;
    const qreal dpr = 2.0;
    QPixmap pm(int(d * dpr), int(d * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c);
    pen.setWidthF(1.35);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == 0) {
        // 산소 방울 — 위 꼭짓점에서 좌우로 벌어져 아래에서 원호로 만나는 형태.
        // 혈중 산소포화도의 관용 기호(맥박산소측정기 UI가 공통으로 쓴다).
        QPainterPath path;
        path.moveTo(7.0, 1.6);
        path.cubicTo(7.0, 1.6, 11.6, 6.6, 11.6, 9.1);
        path.arcTo(QRectF(2.4, 4.5, 9.2, 9.2), 0.0, -180.0);
        path.cubicTo(2.4, 6.6, 7.0, 1.6, 7.0, 1.6);
        p.drawPath(path);
    } else {
        // 심전도 파형 — 평탄선에서 QRS 스파이크 하나. 심박의 관용 기호이고
        // 하트(♥)보다 의료 모니터 문맥에 맞는다.
        QPainterPath path;
        path.moveTo(0.8, 7.2);
        path.lineTo(3.4, 7.2);
        path.lineTo(4.6, 4.0);
        path.lineTo(6.2, 10.6);
        path.lineTo(7.6, 7.2);
        path.lineTo(13.2, 7.2);
        p.drawPath(path);
    }
    p.end();
    return pm;
}

}  // namespace

VitalTile::VitalTile(QWidget* parent) : QFrame(parent)
{
    setObjectName("vitalCard");
    // 세로 Fixed — 남는 공간이 타일로 흘러들면 입소자가 적을수록 타일이
    // 거대해진다(v1의 실제 증상). 타일은 내용만큼만 높고, 남는 세로는
    // 목록 아래에 그대로 비워 둔다.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // v2 VMS 계기판 톤. v1에서 걷어낸 것과 이유:
    //  · 이모지 아이콘(🫁 ❤) — 관제 소프트웨어에 이모지가 들어가면 즉시 아마추어로
    //    읽힌다. 텍스트 캡션(SpO₂ / HR)으로 교체.
    //  · statBox 중첩 — vitalCard(라운드14) 안에 statBox(라운드12)를 넣은 카드-속-카드
    //    구조였다. ROADMAP이 명시적으로 금지한 패턴이고, 밀도를 갉아먹는다.
    //  · 24px 상태 점 — 타일 헤더에 24px 원은 과하다. 타일 왼쪽 끝 세로 레일로
    //    바꿔 등급을 표시한다(면적은 줄고 시선 유도는 오히려 강해진다).
    //
    // 최종 형태 — 좌측 3px 등급 레일 + 3행(이름/판독값/추세):
    //   │ 김영희      채널 1        ✓ 정상
    //   │ SpO₂ 98 %   HR 72 bpm
    //   │ ▁▂▃▅▃▂▁▂▃▅▇▅▃▂
    auto* shell = new QHBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    // 등급 레일 — 기존 dot_ 멤버를 그대로 재사용한다(세터 4곳의 배선 불변).
    // objectName만 #severityDot에서 #vitalRail로 바꾼다: 24px 원거리 계약은
    // 경보 계열 전용이고, 이 레일은 데스크 스케일 요소다(IA-03 펜스 유지).
    dot_ = new QLabel();
    dot_->setObjectName("vitalRail");
    shell->addWidget(dot_);

    auto* lay = new QVBoxLayout();
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    shell->addLayout(lay, 1);

    // ── 1행: 이름 + 병상 + 상태 배지 ──
    auto* head = new QFrame();
    head->setObjectName("vitalHead");
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(11, 6, 9, 5);
    hl->setSpacing(7);
    nameLbl_ = new QLabel();
    nameLbl_->setObjectName("vitalName");
    bedLbl_ = new QLabel();
    bedLbl_->setObjectName("vitalBed");
    badgeLbl_ = new QLabel(QStringLiteral("대기"));
    badgeLbl_->setObjectName("vitalBadge");
    badgeLbl_->setAlignment(Qt::AlignCenter);
    hl->addWidget(nameLbl_);
    hl->addWidget(bedLbl_);
    hl->addStretch();
    hl->addWidget(badgeLbl_);
    lay->addWidget(head);

    // ── 2행: 판독값 2개를 한 줄에. 상자 없이 캡션+수치+단위만 놓는다 ──
    auto* body = new QHBoxLayout();
    body->setContentsMargins(11, 7, 11, 4);
    body->setSpacing(18);

    // 캡션 아이콘 색 — 두 테마 모두에서 읽히는 중간 회색(helpButton과 같은 값).
    const QColor capIcon(0x8B, 0x98, 0xA5);

    auto makeStat = [&](int iconKind, const QString& caption, const QString& unit,
                         QLabel*& valueRef) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(5);
        auto* icon = new QLabel();
        icon->setObjectName("statIcon");
        icon->setPixmap(makeVitalIcon(iconKind, capIcon));
        auto* cap = new QLabel(caption);
        cap->setObjectName("statCaption");
        valueRef = new QLabel(QStringLiteral("--"));
        valueRef->setObjectName("statValue");
        auto* unitLbl = new QLabel(unit);
        unitLbl->setObjectName("statUnit");
        row->addWidget(icon, 0, Qt::AlignBottom);
        row->addWidget(cap, 0, Qt::AlignBottom);
        row->addWidget(valueRef);
        row->addWidget(unitLbl, 0, Qt::AlignBottom);
        return row;
    };

    body->addLayout(makeStat(0, QStringLiteral("SpO₂"), QStringLiteral("%"), spo2Value_));
    body->addLayout(makeStat(1, QStringLiteral("HR"), QStringLiteral("bpm"), hrValue_));
    body->addStretch();
    lay->addLayout(body);

    // ── 3행: 심박 미니 추세 그래프 (고정 스케일 40~140 + 주의/위험 점선) ──
    auto* sparkRow = new QHBoxLayout();
    sparkRow->setContentsMargins(11, 0, 11, 8);
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
