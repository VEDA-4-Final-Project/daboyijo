// MainWindow — 일일 리포트 — 지표, PDF 내보내기, AI 요약
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  일일 리포트 지표 — 선택한 날짜 + 선택한 입소자의 4개 값을 채운다.
//  원천은 전부 서버가 쌓는다: care_logs / bed_sessions / activity_minute / events
// ═══════════════════════════════════════════════════════════
namespace {
// 초 → "8시간 20분 30초" / "4분 27초" / "35초". 0이면 "—"로 비운다.
//
// ★ 초를 버리지 않는다. 케어 세션은 수십 초짜리가 흔해서 분 단위로 자르면
//   "4분 27초"가 "4분"이 되고, 하루치를 합산할 때 사람마다 최대 59초씩 사라진다.
//   대신 값이 0인 자리는 빼서 "1시간 0분 30초" 같은 군더더기는 안 나오게 한다.
QString humanDuration(int sec)
{
    if (sec <= 0) return QStringLiteral("—");

    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;

    QStringList parts;
    if (h > 0) parts << QStringLiteral("%1시간").arg(h);
    if (m > 0) parts << QStringLiteral("%1분").arg(m);
    if (s > 0) parts << QStringLiteral("%1초").arg(s);
    return parts.join(QLatin1Char(' '));
}
}  // namespace

void MainWindow::updateCareTime()
{
    const int rid = reportResidentId_;
    if (!tileCareVal) return;              // 아직 화면이 만들어지기 전

    metrics_ = ReportMetrics{};   // 이번 조회 결과로 새로 채운다(이전 값 잔류 방지)

    // 입소자가 없으면(전원 퇴원 등) 전부 비운다 — 남의 숫자가 남아 있으면 안 된다.
    if (rid <= 0) {
        for (QLabel* l : {tileLyingVal, tileActivityVal, tileCareVal, tileEventVal})
            if (l) l->setText(QStringLiteral("—"));
        for (QLabel* l : {tileLyingSub, tileActivitySub, tileCareSub, tileEventSub})
            if (l) l->setText(QStringLiteral(" "));
        if (activityChart) activityChart->clear();
        return;
    }

    const QDateTime dayStart(reportDate_, QTime(0, 0));
    const QDateTime dayEnd = dayStart.addDays(1);

    // ── 케어시간 ──
    // ★ 하루 구분은 end_time 이 아니라 start_time 기준이다. 서버가 요양사의 잠깐
    //   자리 비움 후 복귀를 직전 행에 합산하면서 end_time 을 뒤로 미는데,
    //   end_time 으로 자르면 자정 직전에 시작한 케어가 통째로 다음 날로 넘어간다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(duration_sec),0), COUNT(*), MAX(end_time) "
            "FROM care_logs WHERE resident_id=? AND DATE(start_time)=?"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec() && q.next()) {
            metrics_.careSec   = q.value(0).toInt();
            metrics_.careCount = q.value(1).toInt();
            tileCareVal->setText(humanDuration(q.value(0).toInt()));
            const QDateTime last = q.value(2).toDateTime();
            if (last.isValid()) metrics_.careLast = last.toString(QStringLiteral("HH:mm"));
            tileCareSub->setText(
                q.value(1).toInt() == 0
                    ? QStringLiteral("기록 없음")
                    : QStringLiteral("%1회 · 마지막 %2")
                          .arg(q.value(1).toInt())
                          .arg(last.isValid() ? last.toString(QStringLiteral("HH:mm"))
                                              : QStringLiteral("—")));
        } else {
            qDebug() << "케어로그 조회 실패:" << q.lastError().text();
        }
    }

    // ── 누워있는 시간 ──
    // ★ 재실 세션은 자정을 넘기는 게 정상이다(23시 취침 → 익일 7시 기상). "시작한
    //   날"로 몰면 안 되고 이 날짜 구간과의 교집합만 세야 한다. 그래서 GREATEST/LEAST
    //   로 양끝을 자른다. 아직 안 닫힌 세션(out_at IS NULL)은 지금 시각까지로 본다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(TIMESTAMPDIFF(SECOND, GREATEST(in_at, ?), "
            "                                  LEAST(COALESCE(out_at, NOW()), ?))), 0), "
            "       COUNT(*) "
            "FROM bed_sessions WHERE resident_id=? "
            "  AND in_at < ? AND COALESCE(out_at, NOW()) > ?"));
        q.addBindValue(dayStart);
        q.addBindValue(dayEnd);
        q.addBindValue(rid);
        q.addBindValue(dayEnd);
        q.addBindValue(dayStart);
        if (q.exec() && q.next()) {
            metrics_.lyingSec   = q.value(0).toInt();
            metrics_.lyingCount = q.value(1).toInt();
            tileLyingVal->setText(humanDuration(q.value(0).toInt()));
            const int n = q.value(1).toInt();
            tileLyingSub->setText(n == 0 ? QStringLiteral("기록 없음")
                                         : QStringLiteral("재실 %1회").arg(n));
        } else {
            qDebug() << "재실 세션 조회 실패:" << q.lastError().text();
        }
    }

    // ── 활동량(만보기) ──
    // 걸음 수는 누적값이라 서버가 1분치 증가분(steps_delta)으로 눌러 담는다.
    // "활동한 분"은 그 분에 10걸음 이상 늘어난 분의 개수다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(steps_delta),0), COALESCE(SUM(steps_delta>=10),0) "
            "FROM activity_minute WHERE resident_id=? AND DATE(minute_ts)=?"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec() && q.next()) {
            const int steps = q.value(0).toInt();
            const int activeMin = q.value(1).toInt();
            metrics_.steps = steps; metrics_.activeMin = activeMin;
            tileActivityVal->setText(steps > 0 ? QStringLiteral("%1걸음").arg(steps)
                                               : QStringLiteral("—"));
            tileActivitySub->setText(steps > 0 ? QStringLiteral("활동 %1분").arg(activeMin)
                                               : QStringLiteral("기록 없음"));
        } else {
            qDebug() << "활동량 조회 실패:" << q.lastError().text();
        }
    }

    // ── 24시간 활동량 그래프 ──
    // 분 단위로 쌓아둔 걸 시간으로 묶어서 24칸을 만든다. 심박은 합계가 아니라
    // 평균이고, 측정 실패(0)는 평균에서 빼야 값이 통째로 내려앉지 않는다.
    if (activityChart) {
        QVector<int> stepsByHour(24, 0);
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT HOUR(minute_ts), COALESCE(SUM(steps_delta),0), "
            "       ROUND(AVG(NULLIF(hr_avg,0))) "
            "FROM activity_minute WHERE resident_id=? AND DATE(minute_ts)=? "
            "GROUP BY HOUR(minute_ts)"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const int h = q.value(0).toInt();
                if (h < 0 || h > 23) continue;
                stepsByHour[h] = q.value(1).toInt();
            }
        } else {
            qDebug() << "시간별 활동량 조회 실패:" << q.lastError().text();
        }
        activityChart->setData(stepsByHour);
    }

    // ── 이벤트 횟수 ──
    // 종류별로 세서 "3회 (낙상1·이탈2)"처럼 내역까지 보여준다. 숫자만 있으면
    // 무슨 일이 있었는지 알 수 없어 리포트로 쓸모가 떨어진다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT event_type, COUNT(*) FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? GROUP BY event_type"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        int total = 0;
        QStringList parts;
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(0).toString();
                const int n = q.value(1).toInt();
                total += n;
                const QString label = t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                                    : t == QLatin1String("EGRESS") ? QStringLiteral("이탈")
                                                                   : QStringLiteral("생체");
                parts << QStringLiteral("%1%2").arg(label).arg(n);
            }
        } else {
            qDebug() << "이벤트 조회 실패:" << q.lastError().text();
        }
        metrics_.eventTotal  = total;
        metrics_.eventDetail = parts.join(QStringLiteral(" · "));
        tileEventVal->setText(total > 0 ? QStringLiteral("%1회").arg(total)
                                        : QStringLiteral("—"));
        tileEventSub->setText(parts.isEmpty() ? QStringLiteral("이벤트 없음")
                                              : parts.join(QStringLiteral(" · ")));
    }

    // 이름·위치는 탭 전환에서 이미 정해진 값을 그대로 쓴다(다시 조회하지 않는다).
    for (int i = 0; i < residentTabBtns.size(); ++i)
        if (residentTabIds.value(i) == rid) metrics_.residentName = residentTabBtns[i]->text();
    if (reportResidentMeta) metrics_.residentMeta = reportResidentMeta->text();
    metrics_.valid = true;
    loadCachedSummary();   // 이 날짜·입소자의 저장된 요약을 표시(API 호출 없음)
}

// ═══════════════════════════════════════════════════════════
//  일일 리포트 PDF 내보내기 — 선택한 날짜·입소자 한 장
//
//  HTML(QTextDocument) → QPdfWriter 로 만든다. QPainter 로 직접 좌표를 찍으면
//  페이지 넘김을 손으로 관리해야 하는데, HTML 은 표·여백을 알아서 흘려주고
//  한글 폰트도 자동으로 임베딩된다.
//  ★ QPdfWriter 는 Qt::Gui 에 있어 모듈 추가가 필요 없다(PrintSupport 불필요).
// ═══════════════════════════════════════════════════════════
void MainWindow::exportReportPdf()
{
    if (!metrics_.valid || reportResidentId_ <= 0) {
        QMessageBox::information(this, QStringLiteral("PDF 내보내기"),
                                 QStringLiteral("먼저 날짜와 입소자를 선택해 주세요."));
        return;
    }

    const QString dateStr = reportDate_.toString(QStringLiteral("yyyy-MM-dd"));
    const QString suggest = QStringLiteral("%1_%2_리포트.pdf")
                                .arg(metrics_.residentName.isEmpty()
                                         ? QStringLiteral("입소자") : metrics_.residentName)
                                .arg(dateStr);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("일일 리포트 저장"),
        QDir::homePath() + QLatin1Char('/') + suggest,
        QStringLiteral("PDF 파일 (*.pdf)"));
    if (path.isEmpty()) return;   // 사용자가 취소

    // ── PDF 장치를 먼저 만든다 ──
    // ★ 해상도를 300dpi 로 낮춰 잡는다. QPdfWriter 기본값은 1200dpi 인데, 그러면
    //   본문 폭이 device 픽셀로 ~9900 이 되어 이미지 폭 같은 픽셀 값을 가늠하기
    //   어렵다. 글자는 pt(해상도 무관)라 300dpi 로도 벡터로 또렷하게 나온다.
    // ★ 이미지 폭을 여기서 구해 HTML 에 넣어야 한다. HTML 의 width 는 device
    //   픽셀이라, 본문 폭을 모르고 700 같은 상수를 쓰면 종이 한구석에만 찍힌다.
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setResolution(300);
    writer.setTitle(QStringLiteral("%1 %2 일일 리포트").arg(metrics_.residentName, dateStr));
    // 실제로 인쇄되는 영역(여백 제외). writer.width() 는 여백을 포함한 종이 전체라
    // 이미지 폭 기준으로 쓰면 오른쪽이 잘린다.
    const QRect bodyPx = writer.pageLayout().paintRectPixels(writer.resolution());
    // ★ HTML 의 width/height 는 96dpi 논리 픽셀로 읽히고, 인쇄할 때 장치 해상도만큼
    //   다시 확대된다(300dpi 면 3.125 배). 이 좌표계를 "1. 요약 지표"가 1페이지에서
    //   차지하는 높이를 재는 데도 똑같이 쓴다 — 서로 다른 좌표계를 섞으면 어긋난다.
    const double dpiScale = writer.resolution() / 96.0;
    const double pageLogicalW = bodyPx.width() / dpiScale;
    const double pageLogicalH = bodyPx.height() / dpiScale;

    // ── 상단(머리말 + "1. 요약 지표") HTML을 먼저 만든다 ──
    // ★ 그래프를 "2. 시간별 활동량" 제목 바로 아래, 같은(첫) 페이지에 넣으려면
    //   그 이미지 높이를 정하기 전에 위 내용이 1페이지에서 이미 얼마를 차지하는지
    //   알아야 한다. 그래서 상단부만 먼저 문자열로 만들어 QTextDocument 로 한 번
    //   재보고, 남는 공간만큼만 그래프에 배정한다.
    const QString dateAndDow = QStringLiteral("%1 (%2)")
        .arg(reportDate_.toString(QStringLiteral("yyyy년 M월 d일")), koreanDow(reportDate_));
    const QString careLastStr = metrics_.careLast.isEmpty() ? QString()
        : QStringLiteral("· 마지막 %1").arg(metrics_.careLast);
    const QString eventDetailStr = metrics_.eventDetail.isEmpty()
        ? QStringLiteral("이벤트 없음") : metrics_.eventDetail;
    const QString generatedStr =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    // 문서번호 — 같은 입소자·같은 날짜면 항상 같은 값이라 재발행해도 안 바뀐다.
    const QString docNumberStr = QStringLiteral("%1-%2")
        .arg(reportDate_.toString(QStringLiteral("yyyyMMdd")))
        .arg(reportResidentId_, 4, 10, QLatin1Char('0'));

    const QString htmlTop = QStringLiteral(R"HTML(
<html><body style="font-family:'맑은 고딕',sans-serif; font-size:11pt; color:#1E2A32; line-height:155%">

<table width="100%" cellspacing="0" cellpadding="0" border="0">
  <tr>
    <td><span style="font-size:10pt; color:#5C6B78; letter-spacing:2px">Carenet 요양원 통합 모니터링</span>
        <h1 style="font-size:26pt; margin:2px 0 0 0">일일 리포트</h1></td>
    <td align="right" valign="bottom">
        <span style="font-size:9pt; color:#8B98A5">문서번호 %1<br>생성 %2</span></td>
  </tr>
</table>
<hr style="border:3px solid #C25A10; margin-top:6px">

<table width="100%" cellspacing="0" cellpadding="9" border="0" style="margin-top:10px">
  <tr bgcolor="#FBEAD9">
    <td width="14%"><b>성명</b></td><td width="36%">%3</td>
    <td width="14%"><b>대상일</b></td><td width="36%">%4</td>
  </tr>
  <tr>
    <td><b>구분</b></td><td>%5</td>
    <td><b>작성</b></td><td>자동 기록 (담당자 입력 없음)</td>
  </tr>
</table>

<p style="margin-top:20px; font-size:13pt"><b style="color:#C25A10">1.</b> <b>요약 지표</b></p>
<table width="100%" cellspacing="0" cellpadding="11" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
  <tr bgcolor="#FBEAD9">
    <th align="left" width="25%">누워있는 시간</th><th align="left" width="25%">활동량</th>
    <th align="left" width="25%">케어시간</th><th align="left" width="25%">이벤트</th>
  </tr>
  <tr>
    <td><b style="font-size:17pt; color:#C25A10">%6</b><br><span style="color:#5C6B78">재실 %7회</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%8걸음</b><br><span style="color:#5C6B78">활동 %9분</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%10</b><br><span style="color:#5C6B78">%11회 %12</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%13회</b><br><span style="color:#5C6B78">%14</span></td>
  </tr>
  <tr style="color:#5C6B78">
    <td>침대에 계셨던 시간의 합계</td>
    <td>웨어러블이 센 걸음 수와 실제로 움직인 시간</td>
    <td>요양사가 병실에 머문 시간 중 침대에 계셨던 시간</td>
    <td>낙상 · 침상이탈 · 생체신호 이상 감지 횟수</td>
  </tr>
</table>
)HTML")
        // ★ QString::arg 의 다중 인자 오버로드는 최대 9개다. 14개를 9 + 5 로 나눈다.
        .arg(docNumberStr, generatedStr, metrics_.residentName, dateAndDow, metrics_.residentMeta,
             humanDuration(metrics_.lyingSec), QString::number(metrics_.lyingCount),
             QString::number(metrics_.steps), QString::number(metrics_.activeMin))
        .arg(humanDuration(metrics_.careSec), QString::number(metrics_.careCount), careLastStr,
             QString::number(metrics_.eventTotal), eventDetailStr);

    // ── 1페이지에 남는 공간 재기 ──
    // ★ 아래 doc.print(&writer) 는 pageSize 를 지정하지 않은 채 인쇄 장치 DPI 로
    //   다시 배치한다(맨 아래 주석 참고). 여기서도 똑같이 pageSize 는 주지 않고
    //   textWidth 만 페이지 폭(96dpi 논리 픽셀)으로 맞춰 같은 방식으로 흘려본다.
    double usedLogicalH = 0.0;
    {
        QTextDocument measureDoc;
        measureDoc.setHtml(htmlTop + QStringLiteral("</body></html>"));
        measureDoc.setTextWidth(pageLogicalW);
        usedLogicalH = measureDoc.size().height();
    }
    // "2." 제목 + 설명 두 줄이 차지할 높이는 대략치로 미리 떼어 둔다. 너무 빡빡하게
    // 잡으면 이 어림치가 틀렸을 때 그래프가 다시 다음 페이지로 밀려나므로, 살짝
    // 넉넉하게 잡는다(1페이지 아래쪽에 여백이 조금 남는 편이 그래프가 잘리는 것보다 낫다).
    const double captionReserve = 90.0;
    // 이보다 좁으면 억지로 욱여넣지 않고 그래프를 다음 페이지로 넘긴다 — 너무
    // 작은 그래프는 안 넣느니만 못하다.
    const double minChartLogicalH = 110.0;
    const double avail = pageLogicalH - usedLogicalH - captionReserve;
    const bool chartNeedsNewPage = avail < minChartLogicalH;
    // 다음 페이지로 넘어가면 페이지 전체를 쓸 수 있으니 기존처럼 1/3 로 잡고,
    // 1페이지에 이어 붙일 땐 실제로 남는 만큼만 배정한다.
    const double chartBudgetLogical = chartNeedsNewPage ? pageLogicalH / 3.0 : avail;

    // ── 그래프를 이미지로 ──
    // ★ 색: 화면은 다크 테마일 수 있는데 종이는 흰색이다. 그대로 뜨면 어두운 상자가
    //   찍히고 회색 눈금이 잘 안 보인다. 그리는 동안만 팔레트를 라이트로 바꿔
    //   렌더하고 곧바로 되돌린다(같은 스레드에서 동기 렌더라 화면에는 영향 없다).
    // ★ 해상도: grab() 은 화면 dpi 로 떠서 300dpi 종이에선 흐리다. 3배로 렌더한다.
    QString chartHtml;
    if (activityChart && activityChart->width() > 0) {
        constexpr int kScale = 3;
        QPixmap shot(activityChart->size() * kScale);

        const Palette saved{kBgDeep, kPanel, kCard, kBorder, kTextMain, kTextSub,
                            kAccent, kOnAccent, kNormal, kWarn, kHigh, kCritical,
                            kInfo, kSelect, kOnSelect};
        applyPalette(kLight);
        shot.fill(Qt::white);
        QPainter pp(&shot);
        pp.scale(kScale, kScale);
        activityChart->render(&pp, QPoint(), QRegion(), QWidget::DrawChildren);
        pp.end();
        applyPalette(saved);
        activityChart->update();   // 화면은 원래 팔레트로 다시 그린다

        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (shot.save(&buf, "PNG")) {
            // bodyPx(장치 픽셀)를 그대로 넣으면 인쇄 시 배율만큼 또 커져 종이 밖으로
            // 나간다 — dpiScale 로 나눠 논리 픽셀로 넣는다.
            int w = int(pageLogicalW * 0.98);
            int h = shot.width() > 0 ? w * shot.height() / shot.width() : 0;
            const int hMax = int(chartBudgetLogical);
            if (h > hMax && h > 0) { w = w * hMax / h; h = hMax; }
            // ★ "2." 제목이 이미 "시간별 활동량"이라 이 안에서 또 제목을 달지 않는다
            //   (전엔 여기서 한 번 더 달아 제목이 겹치고, 그만큼 세로 공간도 더 먹었다).
            chartHtml = QStringLiteral(
                "<p style='color:#5C6B78; margin-top:4px'>"
                "막대 하나가 한 시간 동안 걸은 걸음 수입니다. "
                "가장 활발했던 시간대는 진하게 표시됩니다.</p>"
                "<img src='data:image/png;base64,%1' width='%2' height='%3'>")
                .arg(QString::fromLatin1(png.toBase64()))
                .arg(w).arg(h);
        }
    }
    if (chartHtml.isEmpty()) {
        // 그래프를 못 그렸을 때(위젯이 없거나 이 날짜에 걸음 데이터가 없어 폭이 0일 때) —
        // 캡션만 있고 그 아래가 텅 비면 페이지가 어색해 보인다. 그래프가 있었을 때와
        // 비슷한 높이의 빈 칸을 만들고 안내문을 그 칸 한가운데(가로·세로 모두)에 둔다.
        const int hMax = int(chartBudgetLogical);
        chartHtml = QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0' border='0'>"
            "<tr><td align='center' valign='middle' height='%1' "
            "style='color:#8B98A5'>표시할 활동량 데이터가 없습니다</td></tr></table>")
            .arg(hMax);
    }

    // ── 이벤트 목록 ──
    // 숫자만 있으면 "새벽 3시"인지 "낮 3시"인지 알 수 없다. 시각이 있어야
    // 보호자가 읽을 수 있는 문서가 된다.
    QString eventRows;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT TIME(occurred_at), event_type, source, confirmed_at FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(reportResidentId_);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(1).toString();
                const QString label = t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                                    : t == QLatin1String("EGRESS") ? QStringLiteral("침상이탈")
                                                                   : QStringLiteral("생체신호 이상");
                eventRows += QStringLiteral(
                    "<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                    .arg(q.value(0).toString().left(5), label,
                         q.value(2).toString() == QLatin1String("WEARABLE")
                             ? QStringLiteral("웨어러블") : QStringLiteral("카메라"),
                         q.value(3).isNull() ? QStringLiteral("미확인")
                                             : QStringLiteral("확인됨"));
            }
        }
    }
    if (eventRows.isEmpty())
        eventRows = QStringLiteral("<tr><td colspan='4'>이벤트 없음</td></tr>");

    // ── AI 요약 (있을 때만) ──
    // PDF 는 AI 요약에 의존하지 않는다. 생성해 둔 적이 있으면 넣고, 없으면 그 칸 없이
    // 나온다 — 요약이 없다고 리포트를 못 뽑으면 안 되기 때문이다.
    // ★ 회색 상자로 감싸 위쪽 수치와 시각적으로 떼어 놓는다. 위 숫자는 기계가 센
    //   값이고 이 문단은 생성된 글이라, 감사(監査) 상황에서 그 구분이 중요하다.
    QString summaryHtml;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT summary_text, model, generated_at FROM daily_reports "
            "WHERE report_date=? AND resident_id=?"));
        q.addBindValue(reportDate_);
        q.addBindValue(reportResidentId_);
        if (q.exec() && q.next() && !q.value(0).toString().trimmed().isEmpty()) {
            summaryHtml = QStringLiteral(
                "<p style='margin-top:24px'><b>요약</b></p>"
                "<table width='100%' cellspacing='0' cellpadding='10' border='1'"
                "       bordercolor='#DCE4EC'><tr bgcolor='#F0F4F8'><td>%1"
                "<br><span style='color:#8B98A5; font-size:8pt'>"
                "%2 자동 생성 · 위 수치를 근거로 작성되었습니다</span>"
                "</td></tr></table>")
                .arg(q.value(0).toString().toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                   QStringLiteral("<br>")),
                     q.value(2).toDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
    }

    // ── 나머지("2. 시간별 활동량" 이후) HTML ──
    // ★ "2." 제목은 위에서 잰 남는 공간이 너무 좁을 때만(chartNeedsNewPage) 다음
    //   페이지로 넘긴다. 넉넉하면 style 이 빈 문자열이라 "1. 요약 지표" 바로
    //   아래, 1페이지 안에서 그대로 이어 그려진다.
    const QString sectionBreakStyle = chartNeedsNewPage
        ? QStringLiteral("page-break-before:always; ") : QString();

    const QString htmlRest = QStringLiteral(R"HTML(
<p style="%1font-size:13pt"><b style="color:#C25A10">2.</b> <b>시간별 활동량</b></p>
%2

<p style="page-break-before:always; font-size:13pt"><b style="color:#C25A10">3.</b> <b>이벤트 내역</b></p>
<p style="color:#5C6B78; margin-top:2px">
  감지된 시각과 종류입니다. ‘확인’은 관제 담당자가 영상을 확인해 처리한 건입니다.
</p>
<table width="100%" cellspacing="0" cellpadding="10" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
  <tr bgcolor="#FBEAD9"><th align="left" width="14%">시각</th><th align="left" width="30%">종류</th>
      <th align="left" width="28%">감지 방식</th><th align="left" width="28%">확인 여부</th></tr>
  %3
</table>
%4

<p style="margin-top:30px; font-size:10pt"><b>측정 방식 안내</b></p>
<table width="100%" cellspacing="0" cellpadding="8" border="1" bordercolor="#DCE4EC">
  <tr><td style="color:#5C6B78; font-size:9pt; line-height:150%">
    누워있는 시간과 케어시간은 병실 카메라가 침대 영역을 기준으로 판단합니다.
    침대를 벗어나 의자 등에서 보낸 시간은 포함되지 않을 수 있습니다.<br>
    활동량과 생체신호는 손목 웨어러블에서 수집합니다.<br>
    기기 특성상 실제와 차이가 있을 수 있으며, <b>의료적 판단의 근거로 사용하지 마십시오.</b>
  </td></tr>
</table>
<p style="margin-top:14px; color:#8B98A5; font-size:8pt">
  Carenet 요양원 통합 모니터링 &middot; 이 문서는 시스템이 자동 생성했습니다.
</p>
</body></html>)HTML")
        .arg(sectionBreakStyle, chartHtml, eventRows, summaryHtml);

    // ★ doc.setPageSize 를 직접 주지 않는다. 주면 QTextDocument 가 화면 DPI 로
    //   배치된 채 고해상도 페이지에 그대로 얹혀 글자가 1/10 크기로 나온다.
    //   비워 두면 print() 가 문서를 복제해 인쇄 장치 DPI 로 다시 배치해 준다.
    QTextDocument doc;
    doc.setHtml(htmlTop + htmlRest);
    doc.print(&writer);

    QMessageBox::information(this, QStringLiteral("PDF 내보내기"),
                             QStringLiteral("저장했습니다.\n%1").arg(path));
}

// ═══════════════════════════════════════════════════════════
//  AI 요약 — Gemini 로 리포트 한 문단 만들기
//
//  ★ 숫자는 AI 가 만들지 않는다. SQL 이 만든 집계값(metrics_)을 그대로 넘기고
//    AI 는 문장만 쓴다. 리포트는 요양 기록물이라 같은 날짜를 두 번 열었을 때
//    값이 달라지면 안 되고, "이 낙상 1회가 어디서 나왔나"에 답할 수 있어야 한다.
//  ★ 결과는 daily_reports 에 캐시한다. 같은 이유(재현성) + API 비용·지연 절감.
// ═══════════════════════════════════════════════════════════


namespace {
// Gemini 모델.
// ★ "gemini-flash-latest" 같은 별칭 대신 구체 버전을 쓴다. 별칭은 트래픽이 몰리는
//   공용 풀로 라우팅돼 503(This model is currently experiencing high demand)이 잦다.
//   버전을 못박으면 대체로 더 안정적으로 붙는다. 대신 모델이 은퇴하면 직접 올려야
//   하므로, 404(model not found)가 뜨면 이 값을 최신 버전으로 바꾼다 — 다행히
//   Google 이 404 응답에 "use models/... instead" 로 대체 모델을 알려준다.
// 서버(gemini_client / cameras.conf)도 별도로 모델을 갖고 있다 — 바꿀 땐 양쪽 같이.
constexpr const char* kGeminiModel = "gemini-3.6-flash";
}  // namespace
// 키 우선순위: 소스에 박은 값 → 빌드에 박은 값(DABOYIJO_GEMINI_KEY) → QSettings.
//
// ⚠ 이 저장소는 공개(public) GitHub repo다. 아래 kHardcodedGeminiKey 에 실제 키를
//   적고 커밋/푸시하면 그 순간 키가 인터넷에 노출되고, 히스토리에서 지워도 이미
//   남에게 긁혀갔을 수 있다 — 과금성 남용으로 이어질 수 있다는 점을 알고 쓸 것.
//   (요청에 따라 CMake 설정 없이 바로 쓸 수 있게 이 자리에 하드코딩 경로를 열어둠)
namespace {
constexpr const char* kHardcodedGeminiKey = "AIzaSyCfWoiYaFBOngxCAJDkt5z5t-FCQEhHp_w";  // 여기에 실제 Gemini API 키를 적으세요
}  // namespace
QString MainWindow::geminiApiKey() const
{
    const QString hardcoded = QString::fromLatin1(kHardcodedGeminiKey).trimmed();
    if (!hardcoded.isEmpty()) {
        qDebug() << "[AI] 소스 하드코딩 키 사용, 길이:" << hardcoded.size();
        return hardcoded;
    }
#ifdef DABOYIJO_GEMINI_KEY
    const QString baked = QString::fromLatin1(DABOYIJO_GEMINI_KEY).trimmed();
    // 키 자체는 절대 찍지 않는다 — 길이만으로 "정의가 왔는지/비었는지"가 갈린다.
    qDebug() << "[AI] 빌드 주입 키 길이:" << baked.size();
    if (!baked.isEmpty()) return baked;
#else
    qDebug() << "[AI] DABOYIJO_GEMINI_KEY 매크로 자체가 정의되지 않음 — CMake 재실행 필요";
#endif
    const QString fromSettings =
        QSettings().value(QStringLiteral("ai/geminiKey")).toString().trimmed();
    qDebug() << "[AI] QSettings 폴백 키 길이:" << fromSettings.size();
    return fromSettings;
}

void MainWindow::setSummaryText(const QString& text, bool cached)
{
    if (!summaryLabel) return;
    if (text.isEmpty()) {
        summaryLabel->setText(
            QStringLiteral("요약이 아직 없습니다. [요약] 버튼을 눌러 생성하세요."));
        summaryLabel->setProperty("empty", true);
    } else {
        // 생성된 문장임을 눈에 보이게 표시한다 — 위쪽 숫자는 기계가 센 값이고
        // 이 문단은 AI 가 쓴 글이라는 구분이 감사(監査) 상황에서 중요하다.
        summaryLabel->setText(cached ? text
                                     : QStringLiteral("%1").arg(text));
        summaryLabel->setProperty("empty", false);
    }
    summaryLabel->style()->unpolish(summaryLabel);
    summaryLabel->style()->polish(summaryLabel);
    if (summaryBtn)
        summaryBtn->setText(text.isEmpty() ? QStringLiteral("요약")
                                           : QStringLiteral("요약 다시 생성"));
}

// 날짜·입소자가 바뀔 때마다 캐시를 먼저 보여준다(API 호출 없음).
void MainWindow::loadCachedSummary()
{
    if (!summaryLabel) return;
    if (reportResidentId_ <= 0) { setSummaryText(QString(), true); return; }

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT summary_text FROM daily_reports WHERE report_date=? AND resident_id=?"));
    q.addBindValue(reportDate_);
    q.addBindValue(reportResidentId_);
    setSummaryText((q.exec() && q.next()) ? q.value(0).toString() : QString(), true);
}

void MainWindow::requestAiSummary()
{
    if (summaryBusy_) return;                       // 응답 오기 전 중복 클릭 방지
    if (!metrics_.valid || reportResidentId_ <= 0) {
        QMessageBox::information(this, QStringLiteral("요약"),
                                 QStringLiteral("먼저 날짜와 입소자를 선택해 주세요."));
        return;
    }
    const QString key = geminiApiKey();
    if (key.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("요약"),
            QStringLiteral("요약 기능이 이 빌드에 설정되지 않았습니다.\n\n"
                           "빌드할 때 DABOYIJO_GEMINI_KEY 를 지정해야 합니다.\n"
                           "(Qt Creator: 프로젝트 > 빌드 설정 > CMake)\n\n"
                           "나머지 리포트 기능은 그대로 사용할 수 있습니다."));
        return;
    }

    // ── 프롬프트: 집계 수치만 ──
    // 원본 로그(개별 이벤트 행, 분당 걸음)는 보내지 않는다. 요약에 필요하지도 않고
    // 개인정보를 넓게 노출할 이유도 없다.
    QString events;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT TIME(occurred_at), event_type FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(reportResidentId_);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(1).toString();
                events += QStringLiteral("%1 %2, ").arg(
                    q.value(0).toString().left(5),
                    t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                  : t == QLatin1String("EGRESS") ? QStringLiteral("침상이탈")
                                                 : QStringLiteral("생체신호 이상"));
            }
        }
    }
    if (events.isEmpty()) events = QStringLiteral("없음");

    const QString prompt = QStringLiteral(
        "당신은 요양원 간호기록을 쓰는 담당자입니다. 아래는 입소자 한 명의 하루 기록입니다.\n"
        "이 수치만 근거로 보호자가 읽을 3~4문장 요약을 한국어로 작성하세요.\n\n"
        "[규칙]\n"
        "- 주어진 수치 외의 어떤 숫자도 만들어내지 마세요. 특히 '평소 대비', '지난주보다' 같은\n"
        "  비교는 비교할 자료가 없으므로 절대 쓰지 마세요.\n"
        "- 의학적 진단이나 처방을 하지 마세요. 관찰된 사실과 그 의미만 서술하세요.\n"
        "- 이벤트가 있었다면 시각과 함께 먼저 언급하세요.\n"
        "- 문장만 출력하세요. 제목·머리말·목록 기호는 넣지 마세요.\n\n"
        "[기록]\n"
        "날짜: %1\n입소자: %2 (%3)\n"
        "누워있던 시간: %4 (재실 %5회)\n"
        "활동량: %6걸음 (움직인 시간 %7분)\n"
        "요양사 케어: %8 (%9회)\n"
        "이벤트: %10")
        .arg(reportDate_.toString(QStringLiteral("yyyy년 M월 d일")),
             metrics_.residentName, metrics_.residentMeta,
             humanDuration(metrics_.lyingSec), QString::number(metrics_.lyingCount),
             QString::number(metrics_.steps), QString::number(metrics_.activeMin),
             humanDuration(metrics_.careSec), QString::number(metrics_.careCount))
        .arg(events);

    // ── Gemini 호출 ──
    // 서버(gemini_client.cpp)와 같은 엔드포인트를 쓴다. 모델을 바꾸려면 양쪽을 같이.
    QJsonObject part;   part.insert(QStringLiteral("text"), prompt);
    QJsonObject content; content.insert(QStringLiteral("parts"), QJsonArray{part});
    QJsonObject body;    body.insert(QStringLiteral("contents"), QJsonArray{content});

    QNetworkRequest req(QUrl(QStringLiteral(
        "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
            .arg(QLatin1String(kGeminiModel))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", key.toUtf8());

    summaryBusy_ = true;
    if (summaryBtn) summaryBtn->setEnabled(false);
    if (summaryLabel) summaryLabel->setText(QStringLiteral("요약을 생성하는 중입니다…"));

    // 응답이 오는 사이 사용자가 날짜·입소자를 바꿀 수 있다. 그때 엉뚱한 리포트에
    // 요약이 붙지 않도록 요청 시점의 대상을 캡처해 두고 도착 시 비교한다.
    const QDate reqDate = reportDate_;
    const int   reqRid  = reportResidentId_;

    auto* nam = new QNetworkAccessManager(this);
    QNetworkReply* reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, nam, reqDate, reqRid]() {
        reply->deleteLater();
        nam->deleteLater();
        summaryBusy_ = false;
        if (summaryBtn) summaryBtn->setEnabled(true);

        if (reply->error() != QNetworkReply::NoError) {
            // Gemini 는 실패해도 본문에 {"error":{"message":"..."}} 를 담아 보낸다.
            // 상태 코드만 보면 "503" 밖에 안 나와 원인을 알 수 없으므로 본문을 읽는다.
            const QByteArray raw = reply->readAll();
            const QJsonObject err = QJsonDocument::fromJson(raw).object()
                                        .value(QStringLiteral("error")).toObject();
            const QString detail = err.value(QStringLiteral("message")).toString();
            const int code = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();

            // 503/429 는 모델 과부하·쿼터라 대개 잠시 뒤 다시 누르면 된다.
            const QString hint =
                (code == 503) ? QStringLiteral(" (모델 과부하 — 잠시 후 다시 시도해 보세요)")
              : (code == 429) ? QStringLiteral(" (요청 한도 초과 — 잠시 후 다시 시도해 보세요)")
              : (code == 400 || code == 403)
                    ? QStringLiteral(" (API 키를 확인해 주세요)")
                    : QString();

            const QString msg = detail.isEmpty() ? reply->errorString() : detail;
            qDebug() << "AI 요약 실패:" << code << msg << raw;
            if (summaryLabel)
                summaryLabel->setText(QStringLiteral("요약 생성 실패: %1%2").arg(msg, hint));
            return;
        }

        // candidates[0].content.parts[0].text 를 꺼낸다(서버 구현과 같은 경로).
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray cands = root.value(QStringLiteral("candidates")).toArray();
        QString text;
        if (!cands.isEmpty()) {
            const QJsonArray parts = cands.first().toObject()
                                          .value(QStringLiteral("content")).toObject()
                                          .value(QStringLiteral("parts")).toArray();
            if (!parts.isEmpty())
                text = parts.first().toObject().value(QStringLiteral("text")).toString().trimmed();
        }
        if (text.isEmpty()) {
            if (summaryLabel)
                summaryLabel->setText(QStringLiteral("요약 응답을 해석하지 못했습니다."));
            return;
        }

        // 캐시는 요청 시점 대상에 저장한다(그 사이 화면이 바뀌었어도 기록은 제자리로).
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "INSERT INTO daily_reports (report_date, resident_id, summary_text, model, generated_at) "
            "VALUES (?,?,?,?,NOW()) "
            "ON DUPLICATE KEY UPDATE summary_text=VALUES(summary_text), "
            "model=VALUES(model), generated_at=VALUES(generated_at)"));
        q.addBindValue(reqDate);
        q.addBindValue(reqRid);
        q.addBindValue(text);
        q.addBindValue(QLatin1String(kGeminiModel));
        if (!q.exec()) qDebug() << "AI 요약 캐시 저장 실패:" << q.lastError().text();

        // 화면이 아직 그 리포트를 보고 있을 때만 표시한다.
        if (reqDate == reportDate_ && reqRid == reportResidentId_)
            setSummaryText(text, false);
    });
}
