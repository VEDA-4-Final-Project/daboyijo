#include "reportbackend.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextDocument>
#include <QUrl>

namespace {
constexpr const char* kGeminiModel = "gemini-3.6-flash";

QString koreanDow(const QDate& d)
{
    static const char* kDow[] = {"월", "화", "수", "목", "금", "토", "일"};
    const int i = d.dayOfWeek() - 1;
    return (i >= 0 && i < 7) ? QString::fromUtf8(kDow[i]) : QString();
}

// 초 → "8시간 20분 30초". 0 이면 "—". 초를 버리지 않는다(케어 세션은 수십 초짜리가 흔하다).
QString humanDuration(int sec)
{
    if (sec <= 0) return QStringLiteral("—");
    const int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    QStringList p;
    if (h > 0) p << QStringLiteral("%1시간").arg(h);
    if (m > 0) p << QStringLiteral("%1분").arg(m);
    if (s > 0) p << QStringLiteral("%1초").arg(s);
    return p.join(QLatin1Char(' '));
}

QString eventLabel(const QString& t, bool longForm)
{
    if (t == QLatin1String("FALL")) return QStringLiteral("낙상");
    if (t == QLatin1String("EGRESS")) return longForm ? QStringLiteral("침상이탈") : QStringLiteral("이탈");
    return longForm ? QStringLiteral("생체신호 이상") : QStringLiteral("생체");
}

QVariantMap tile(const QString& val, const QString& sub) { return {{"val", val}, {"sub", sub}}; }
}  // namespace

ReportBackend::ReportBackend(QObject* parent)
    : QObject(parent), net_(new QNetworkAccessManager(this)), date_(QDate::currentDate())
{
    stepsByHour_ = QVector<int>(24, 0);
}

QString ReportBackend::dateLabel() const
{
    const QString when = QStringLiteral("%1 (%2)").arg(date_.toString("yyyy-MM-dd"), koreanDow(date_));
    return date_ == QDate::currentDate() ? QStringLiteral("%1 · 오늘").arg(when) : when;
}

void ReportBackend::setDate(const QString& s)
{
    QDate d = QDate::fromString(s, Qt::ISODate);
    if (!d.isValid() || d == date_) return;
    if (d > QDate::currentDate()) d = QDate::currentDate();
    date_ = d;
    emit dateChanged();
    update();
}

void ReportBackend::today() { setDate(QDate::currentDate().toString(Qt::ISODate)); }

void ReportBackend::reloadResidents()
{
    residents_.clear();
    QVector<int> ids;
    QSqlQuery q;
    if (!q.exec(QStringLiteral("SELECT resident_id, name FROM residents WHERE status='재원' "
                               "ORDER BY camera_id, bed, resident_id"))) {
        qDebug() << "리포트 입소자 목록 조회 실패:" << q.lastError().text();
    } else {
        while (q.next()) {
            residents_ << QVariantMap{{"id", q.value(0).toInt()}, {"name", q.value(1).toString()}};
            ids << q.value(0).toInt();
        }
    }
    emit residentsChanged();
    if (!ids.contains(rid_)) rid_ = ids.isEmpty() ? -1 : ids.first();
    setResidentId(rid_);
}

void ReportBackend::setResidentId(int id)
{
    rid_ = id;
    m_.residentMeta = QStringLiteral("—");
    m_.residentName.clear();
    for (const auto& v : residents_)
        if (v.toMap().value("id").toInt() == id) m_.residentName = v.toMap().value("name").toString();
    if (id > 0) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT room, bed, COALESCE(risk_level,'—') FROM residents WHERE resident_id=?"));
        q.addBindValue(id);
        if (q.exec() && q.next()) {
            QStringList parts;
            const QString room = q.value(0).toString().trimmed(), bed = q.value(1).toString().trimmed();
            if (!room.isEmpty()) parts << QStringLiteral("%1호").arg(room);
            if (!bed.isEmpty()) parts << bed;
            parts << QStringLiteral("위험도 %1").arg(q.value(2).toString());
            m_.residentMeta = parts.join(QStringLiteral(" · "));
        }
    }
    update();
}

void ReportBackend::update()
{
    const QString keepName = m_.residentName, keepMeta = m_.residentMeta;
    m_ = Metrics{};
    m_.residentName = keepName;
    m_.residentMeta = keepMeta;
    stepsByHour_ = QVector<int>(24, 0);
    const QString empty = QStringLiteral("—"), blank = QStringLiteral(" ");
    tiles_ = {{"lying", tile(empty, blank)}, {"activity", tile(empty, blank)},
              {"care", tile(empty, blank)}, {"event", tile(empty, blank)}};

    if (rid_ <= 0) {
        hours_.clear();
        emit metricsChanged();
        setSummary(QString());
        return;
    }
    const QDateTime dayStart(date_, QTime(0, 0));
    const QDateTime dayEnd = dayStart.addDays(1);

    // 케어시간 — 하루 구분은 end_time 이 아니라 start_time 기준
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT COALESCE(SUM(duration_sec),0), COUNT(*), MAX(end_time) "
                                 "FROM care_logs WHERE resident_id=? AND DATE(start_time)=?"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        if (q.exec() && q.next()) {
            m_.careSec = q.value(0).toInt();
            m_.careCount = q.value(1).toInt();
            const QDateTime last = q.value(2).toDateTime();
            if (last.isValid()) m_.careLast = last.toString("HH:mm");
            tiles_["care"] = tile(humanDuration(m_.careSec),
                m_.careCount == 0 ? QStringLiteral("기록 없음")
                                  : QStringLiteral("%1회 · 마지막 %2").arg(m_.careCount)
                                        .arg(last.isValid() ? last.toString("HH:mm") : QStringLiteral("—")));
        } else qDebug() << "케어로그 조회 실패:" << q.lastError().text();
    }
    // 누워있는 시간 — 자정을 넘기는 세션은 이 날짜 구간과의 교집합만
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(TIMESTAMPDIFF(SECOND, GREATEST(in_at, ?), LEAST(COALESCE(out_at, NOW()), ?))), 0), "
            "       COUNT(*) FROM bed_sessions WHERE resident_id=? AND in_at < ? AND COALESCE(out_at, NOW()) > ?"));
        q.addBindValue(dayStart); q.addBindValue(dayEnd); q.addBindValue(rid_);
        q.addBindValue(dayEnd); q.addBindValue(dayStart);
        if (q.exec() && q.next()) {
            m_.lyingSec = q.value(0).toInt();
            m_.lyingCount = q.value(1).toInt();
            tiles_["lying"] = tile(humanDuration(m_.lyingSec),
                m_.lyingCount == 0 ? QStringLiteral("기록 없음") : QStringLiteral("재실 %1회").arg(m_.lyingCount));
        } else qDebug() << "재실 세션 조회 실패:" << q.lastError().text();
    }
    // 활동량 — 활동한 분 = 그 분에 10걸음 이상
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT COALESCE(SUM(steps_delta),0), COALESCE(SUM(steps_delta>=10),0) "
                                 "FROM activity_minute WHERE resident_id=? AND DATE(minute_ts)=?"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        if (q.exec() && q.next()) {
            m_.steps = q.value(0).toInt();
            m_.activeMin = q.value(1).toInt();
            tiles_["activity"] = tile(m_.steps > 0 ? QStringLiteral("%1걸음").arg(m_.steps) : empty,
                                      m_.steps > 0 ? QStringLiteral("활동 %1분").arg(m_.activeMin) : QStringLiteral("기록 없음"));
        } else qDebug() << "활동량 조회 실패:" << q.lastError().text();
    }
    // 시간별 걸음 합계(24칸)
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT HOUR(minute_ts), COALESCE(SUM(steps_delta),0) FROM activity_minute "
                                 "WHERE resident_id=? AND DATE(minute_ts)=? GROUP BY HOUR(minute_ts)"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        if (q.exec()) {
            while (q.next()) {
                const int h = q.value(0).toInt();
                if (h >= 0 && h < 24) stepsByHour_[h] = q.value(1).toInt();
            }
        } else qDebug() << "시간별 활동량 조회 실패:" << q.lastError().text();
    }
    hours_.clear();
    for (int v : stepsByHour_) hours_ << v;
    // 이벤트 횟수
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT event_type, COUNT(*) FROM events WHERE resident_id=? AND DATE(occurred_at)=? GROUP BY event_type"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        int total = 0;
        QStringList parts;
        if (q.exec()) {
            while (q.next()) {
                const int n = q.value(1).toInt();
                total += n;
                parts << QStringLiteral("%1%2").arg(eventLabel(q.value(0).toString(), false)).arg(n);
            }
        } else qDebug() << "이벤트 조회 실패:" << q.lastError().text();
        m_.eventTotal = total;
        m_.eventDetail = parts.join(QStringLiteral(" · "));
        tiles_["event"] = tile(total > 0 ? QStringLiteral("%1회").arg(total) : empty,
                               parts.isEmpty() ? QStringLiteral("이벤트 없음") : m_.eventDetail);
    }
    m_.valid = true;
    emit metricsChanged();
    loadCachedSummary();
}

void ReportBackend::setSummary(const QString& t)
{
    summary_ = t;
    hasSummary_ = !t.isEmpty();
    emit summaryChanged();
}

void ReportBackend::loadCachedSummary()
{
    if (rid_ <= 0) { setSummary(QString()); return; }
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT summary_text FROM daily_reports WHERE report_date=? AND resident_id=?"));
    q.addBindValue(date_);
    q.addBindValue(rid_);
    setSummary((q.exec() && q.next()) ? q.value(0).toString() : QString());
}

// 키는 소스에 박지 않는다 — 빌드 주입(DABOYIJO_GEMINI_KEY) → QSettings("ai/geminiKey") 순.
QString ReportBackend::key()
{
#ifdef DABOYIJO_GEMINI_KEY
    const QString baked = QString::fromLatin1(DABOYIJO_GEMINI_KEY).trimmed();
    if (!baked.isEmpty()) return baked;
#endif
    return QSettings().value(QStringLiteral("ai/geminiKey")).toString().trimmed();
}

void ReportBackend::requestSummary()
{
    if (busy_) return;
    if (!m_.valid || rid_ <= 0) { emit notice(QStringLiteral("요약"), QStringLiteral("먼저 날짜와 입소자를 선택해 주세요.")); return; }
    const QString k = key();
    if (k.isEmpty()) {
        emit notice(QStringLiteral("요약"),
                    QStringLiteral("요약 기능이 이 빌드에 설정되지 않았습니다.\n\n"
                                   "빌드할 때 DABOYIJO_GEMINI_KEY 를 지정하거나 설정(ai/geminiKey)에 키를 넣어야 합니다.\n\n"
                                   "나머지 리포트 기능은 그대로 사용할 수 있습니다."));
        return;
    }
    QString events;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT TIME(occurred_at), event_type FROM events WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        if (q.exec())
            while (q.next())
                events += QStringLiteral("%1 %2, ").arg(q.value(0).toString().left(5), eventLabel(q.value(1).toString(), true));
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
        "[기록]\n날짜: %1\n입소자: %2 (%3)\n누워있던 시간: %4 (재실 %5회)\n"
        "활동량: %6걸음 (움직인 시간 %7분)\n요양사 케어: %8 (%9회)\n이벤트: %10")
        .arg(date_.toString(QStringLiteral("yyyy년 M월 d일")), m_.residentName, m_.residentMeta,
             humanDuration(m_.lyingSec), QString::number(m_.lyingCount),
             QString::number(m_.steps), QString::number(m_.activeMin),
             humanDuration(m_.careSec), QString::number(m_.careCount))
        .arg(events);

    QJsonObject part; part.insert("text", prompt);
    QJsonObject content; content.insert("parts", QJsonArray{part});
    QJsonObject body; body.insert("contents", QJsonArray{content});
    QNetworkRequest req(QUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
                                 .arg(QLatin1String(kGeminiModel))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", k.toUtf8());

    busy_ = true;
    summary_ = QStringLiteral("요약을 생성하는 중입니다…");
    emit summaryChanged();
    const QDate reqDate = date_;
    const int reqRid = rid_;
    QNetworkReply* reply = net_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqDate, reqRid] {
        reply->deleteLater();
        busy_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            const QByteArray raw = reply->readAll();
            const QString detail = QJsonDocument::fromJson(raw).object().value("error").toObject().value("message").toString();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString hint = code == 503 ? QStringLiteral(" (모델 과부하 — 잠시 후 다시 시도해 보세요)")
                               : code == 429 ? QStringLiteral(" (요청 한도 초과 — 잠시 후 다시 시도해 보세요)")
                               : (code == 400 || code == 403) ? QStringLiteral(" (API 키를 확인해 주세요)") : QString();
            summary_ = QStringLiteral("요약 생성 실패: %1%2").arg(detail.isEmpty() ? reply->errorString() : detail, hint);
            hasSummary_ = false;
            emit summaryChanged();
            return;
        }
        const QJsonArray cands = QJsonDocument::fromJson(reply->readAll()).object().value("candidates").toArray();
        QString text;
        if (!cands.isEmpty()) {
            const QJsonArray parts = cands.first().toObject().value("content").toObject().value("parts").toArray();
            if (!parts.isEmpty()) text = parts.first().toObject().value("text").toString().trimmed();
        }
        if (text.isEmpty()) {
            summary_ = QStringLiteral("요약 응답을 해석하지 못했습니다.");
            hasSummary_ = false;
            emit summaryChanged();
            return;
        }
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO daily_reports (report_date, resident_id, summary_text, model, generated_at) "
                                 "VALUES (?,?,?,?,NOW()) ON DUPLICATE KEY UPDATE summary_text=VALUES(summary_text), "
                                 "model=VALUES(model), generated_at=VALUES(generated_at)"));
        q.addBindValue(reqDate); q.addBindValue(reqRid); q.addBindValue(text);
        q.addBindValue(QLatin1String(kGeminiModel));
        if (!q.exec()) qDebug() << "AI 요약 캐시 저장 실패:" << q.lastError().text();
        if (reqDate == date_ && reqRid == rid_) setSummary(text);
        else { summary_.clear(); emit summaryChanged(); loadCachedSummary(); }
    });
}

QString ReportBackend::suggestedPdfName() const
{
    return QStringLiteral("%1_%2_리포트.pdf")
        .arg(m_.residentName.isEmpty() ? QStringLiteral("입소자") : m_.residentName, date_.toString("yyyy-MM-dd"));
}

// PDF 는 라이트 팔레트(흰 종이)로 막대 그래프를 그려 넣는다 — 위젯과 같은 모양.
QImage ReportBackend::renderChart() const
{
    constexpr int W = 640, H = 190, S = 3, padL = 44, padR = 14, padT = 14, padB = 26;
    QImage img(W * S, H * S, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.scale(S, S);
    p.setRenderHint(QPainter::Antialiasing);
    int stepMax = 0;
    for (int v : stepsByHour_) stepMax = qMax(stepMax, v);
    if (stepMax <= 0) {
        p.setPen(QColor("#5C6B78"));
        p.drawText(QRect(0, 0, W, H), Qt::AlignCenter, QStringLiteral("이 날짜의 활동 기록이 없습니다"));
        return img;
    }
    int step = 100;
    while (stepMax > step * 5) step *= 2;
    const int axisMax = ((stepMax + step - 1) / step) * step;
    const QRectF plot(padL, padT, W - padL - padR, H - padT - padB);
    QFont f = p.font(); f.setPointSize(8); p.setFont(f);
    const QFontMetrics fm(f);
    p.setPen(QPen(QColor("#DCE4EC"), 1, Qt::DotLine));
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p.setPen(QColor("#5C6B78"));
    for (int g = 0; g <= 2; ++g) {
        const double y = plot.bottom() - plot.height() * g / 2.0;
        const QString t = QString::number(axisMax * g / 2);
        p.drawText(QPointF(plot.left() - 6 - fm.horizontalAdvance(t), y + fm.ascent() / 2.0 - 1), t);
    }
    const double slot = plot.width() / 24, barW = qMax(2.0, slot * 0.62);
    for (int h = 0; h < 24; ++h) {
        if (stepsByHour_[h] <= 0) continue;
        const double bh = plot.height() * stepsByHour_[h] / double(axisMax);
        QColor c("#12B5A6");
        c.setAlpha(stepsByHour_[h] == stepMax ? 255 : 150);
        p.fillRect(QRectF(plot.left() + slot * h + (slot - barW) / 2.0, plot.bottom() - bh, barW, bh), c);
    }
    p.setPen(QColor("#5C6B78"));
    for (int h = 0; h < 24; h += 3) {
        const QString t = QString::number(h);
        p.drawText(QPointF(plot.left() + slot * h + slot / 2.0 - fm.horizontalAdvance(t) / 2.0, plot.bottom() + fm.ascent() + 5), t);
    }
    return img;
}

QVariantMap ReportBackend::exportPdf(const QString& pathIn)
{
    if (!m_.valid || rid_ <= 0) return {{"ok", false}, {"message", QStringLiteral("먼저 날짜와 입소자를 선택해 주세요.")}};
    QString path = pathIn;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();
    if (path.isEmpty()) return {{"ok", false}, {"message", QString()}};

    const QString dateStr = date_.toString("yyyy-MM-dd");
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setResolution(300);
    writer.setTitle(QStringLiteral("%1 %2 일일 리포트").arg(m_.residentName, dateStr));
    const QRect bodyPx = writer.pageLayout().paintRectPixels(writer.resolution());
    const double dpiScale = writer.resolution() / 96.0;
    const double pageW = bodyPx.width() / dpiScale, pageH = bodyPx.height() / dpiScale;

    const QString dateAndDow = QStringLiteral("%1 (%2)").arg(date_.toString("yyyy년 M월 d일"), koreanDow(date_));
    const QString careLastStr = m_.careLast.isEmpty() ? QString() : QStringLiteral("· 마지막 %1").arg(m_.careLast);
    const QString eventDetail = m_.eventDetail.isEmpty() ? QStringLiteral("이벤트 없음") : m_.eventDetail;
    const QString generated = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    const QString docNo = QStringLiteral("%1-%2").arg(date_.toString("yyyyMMdd")).arg(rid_, 4, 10, QLatin1Char('0'));

    const QString htmlTop = QStringLiteral(R"HTML(
<html><body style="font-family:'맑은 고딕',sans-serif; font-size:11pt; color:#1E2A32; line-height:155%">
<table width="100%" cellspacing="0" cellpadding="0" border="0"><tr>
 <td><span style="font-size:10pt; color:#5C6B78; letter-spacing:2px">Carenet 요양원 통합 모니터링</span>
     <h1 style="font-size:26pt; margin:2px 0 0 0">일일 리포트</h1></td>
 <td align="right" valign="bottom"><span style="font-size:9pt; color:#8B98A5">문서번호 %1<br>생성 %2</span></td></tr></table>
<hr style="border:3px solid #C25A10; margin-top:6px">
<table width="100%" cellspacing="0" cellpadding="9" border="0" style="margin-top:10px">
 <tr bgcolor="#FBEAD9"><td width="14%"><b>성명</b></td><td width="36%">%3</td><td width="14%"><b>대상일</b></td><td width="36%">%4</td></tr>
 <tr><td><b>구분</b></td><td>%5</td><td><b>작성</b></td><td>자동 기록 (담당자 입력 없음)</td></tr></table>
<p style="margin-top:20px; font-size:13pt"><b style="color:#C25A10">1.</b> <b>요약 지표</b></p>
<table width="100%" cellspacing="0" cellpadding="11" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
 <tr bgcolor="#FBEAD9"><th align="left" width="25%">누워있는 시간</th><th align="left" width="25%">활동량</th>
  <th align="left" width="25%">케어시간</th><th align="left" width="25%">이벤트</th></tr>
 <tr><td><b style="font-size:17pt; color:#C25A10">%6</b><br><span style="color:#5C6B78">재실 %7회</span></td>
  <td><b style="font-size:17pt; color:#C25A10">%8걸음</b><br><span style="color:#5C6B78">활동 %9분</span></td>
  <td><b style="font-size:17pt; color:#C25A10">%10</b><br><span style="color:#5C6B78">%11회 %12</span></td>
  <td><b style="font-size:17pt; color:#C25A10">%13회</b><br><span style="color:#5C6B78">%14</span></td></tr>
 <tr style="color:#5C6B78"><td>침대에 계셨던 시간의 합계</td><td>웨어러블이 센 걸음 수와 실제로 움직인 시간</td>
  <td>요양사가 병실에 머문 시간 중 침대에 계셨던 시간</td><td>낙상 · 침상이탈 · 생체신호 이상 감지 횟수</td></tr></table>
)HTML")
        .arg(docNo, generated, m_.residentName, dateAndDow, m_.residentMeta,
             humanDuration(m_.lyingSec), QString::number(m_.lyingCount),
             QString::number(m_.steps), QString::number(m_.activeMin))
        .arg(humanDuration(m_.careSec), QString::number(m_.careCount), careLastStr,
             QString::number(m_.eventTotal), eventDetail);

    double used = 0;
    { QTextDocument d; d.setHtml(htmlTop + "</body></html>"); d.setTextWidth(pageW); used = d.size().height(); }
    const double avail = pageH - used - 90.0;
    const bool newPage = avail < 110.0;
    const double budget = newPage ? pageH / 3.0 : avail;

    QString chartHtml;
    {
        const QImage img = renderChart();
        QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly);
        if (img.save(&buf, "PNG")) {
            int w = int(pageW * 0.98), h = w * img.height() / img.width();
            if (h > int(budget) && h > 0) { w = w * int(budget) / h; h = int(budget); }
            chartHtml = QStringLiteral("<p style='color:#5C6B78; margin-top:4px'>막대 하나가 한 시간 동안 걸은 걸음 수입니다. "
                                       "가장 활발했던 시간대는 진하게 표시됩니다.</p><img src='data:image/png;base64,%1' width='%2' height='%3'>")
                            .arg(QString::fromLatin1(png.toBase64())).arg(w).arg(h);
        }
    }
    QString eventRows;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT TIME(occurred_at), event_type, source, confirmed_at FROM events "
                                 "WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(rid_);
        q.addBindValue(date_);
        if (q.exec())
            while (q.next())
                eventRows += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                    .arg(q.value(0).toString().left(5), eventLabel(q.value(1).toString(), true),
                         q.value(2).toString() == QLatin1String("WEARABLE") ? QStringLiteral("웨어러블") : QStringLiteral("카메라"),
                         q.value(3).isNull() ? QStringLiteral("미확인") : QStringLiteral("확인됨"));
    }
    if (eventRows.isEmpty()) eventRows = QStringLiteral("<tr><td colspan='4'>이벤트 없음</td></tr>");

    QString summaryHtml;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT summary_text, model, generated_at FROM daily_reports WHERE report_date=? AND resident_id=?"));
        q.addBindValue(date_);
        q.addBindValue(rid_);
        if (q.exec() && q.next() && !q.value(0).toString().trimmed().isEmpty())
            summaryHtml = QStringLiteral("<p style='margin-top:24px'><b>요약</b></p>"
                "<table width='100%' cellspacing='0' cellpadding='10' border='1' bordercolor='#DCE4EC'><tr bgcolor='#F0F4F8'><td>%1"
                "<br><span style='color:#8B98A5; font-size:8pt'>%2 자동 생성 · 위 수치를 근거로 작성되었습니다</span></td></tr></table>")
                .arg(q.value(0).toString().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")),
                     q.value(2).toDateTime().toString("yyyy-MM-dd HH:mm"));
    }
    const QString htmlRest = QStringLiteral(R"HTML(
<p style="%1font-size:13pt"><b style="color:#C25A10">2.</b> <b>시간별 활동량</b></p>
%2
<p style="page-break-before:always; font-size:13pt"><b style="color:#C25A10">3.</b> <b>이벤트 내역</b></p>
<p style="color:#5C6B78; margin-top:2px">감지된 시각과 종류입니다. ‘확인’은 관제 담당자가 영상을 확인해 처리한 건입니다.</p>
<table width="100%" cellspacing="0" cellpadding="10" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
 <tr bgcolor="#FBEAD9"><th align="left" width="14%">시각</th><th align="left" width="30%">종류</th>
  <th align="left" width="28%">감지 방식</th><th align="left" width="28%">확인 여부</th></tr>%3</table>
%4
<p style="margin-top:30px; font-size:10pt"><b>측정 방식 안내</b></p>
<table width="100%" cellspacing="0" cellpadding="8" border="1" bordercolor="#DCE4EC"><tr><td style="color:#5C6B78; font-size:9pt; line-height:150%">
 누워있는 시간과 케어시간은 병실 카메라가 침대 영역을 기준으로 판단합니다. 침대를 벗어나 의자 등에서 보낸 시간은 포함되지 않을 수 있습니다.<br>
 활동량과 생체신호는 손목 웨어러블에서 수집합니다.<br>
 기기 특성상 실제와 차이가 있을 수 있으며, <b>의료적 판단의 근거로 사용하지 마십시오.</b></td></tr></table>
<p style="margin-top:14px; color:#8B98A5; font-size:8pt">Carenet 요양원 통합 모니터링 &middot; 이 문서는 시스템이 자동 생성했습니다.</p>
</body></html>)HTML")
        .arg(newPage ? QStringLiteral("page-break-before:always; ") : QString(), chartHtml, eventRows, summaryHtml);

    QTextDocument doc;
    doc.setHtml(htmlTop + htmlRest);
    doc.print(&writer);
    return {{"ok", true}, {"message", QStringLiteral("저장했습니다.\n%1").arg(path)}};
}
