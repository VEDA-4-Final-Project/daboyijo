#include "eventlogbackend.h"
#include "appbackend.h"
#include "caredirectory.h"
#include "streamclient.h"
#include "theme.h"

#include <QDateTime>
#include <QPair>
#include <algorithm>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

namespace {
constexpr int kMaxRows = 500;
constexpr quint16 kClipHttpPort = 5501;   // server/src/main.cpp 의 kClipHttpPort 와 같게
constexpr quint16 kNvrHttpPort  = 5502;
QString hostForChannel(int ch) { return StreamClient::serverHost(StreamClient::serverForChannel(ch)); }
}

EventLogBackend::EventLogBackend(CareDirectory* dir, AppBackend* app, QObject* parent)
    : QObject(parent), dir_(dir), app_(app), net_(new QNetworkAccessManager(this)),
      from_(QDate::currentDate().addDays(-7)), to_(QDate::currentDate()) {}

QString EventLogBackend::roomName() { return CareDirectory::roomName(); }
bool EventLogBackend::dbOpen() const { return QSqlDatabase::database().isOpen(); }

void EventLogBackend::setDateFrom(const QString& s)
{
    const QDate d = QDate::fromString(s, Qt::ISODate);
    if (!d.isValid() || d == from_) return;
    from_ = d; emit filterChanged(); reload();
}
void EventLogBackend::setDateTo(const QString& s)
{
    const QDate d = QDate::fromString(s, Qt::ISODate);
    if (!d.isValid() || d == to_) return;
    to_ = d; emit filterChanged(); reload();
}

void EventLogBackend::preset(int days)
{
    from_ = QDate::currentDate().addDays(-days);
    to_ = QDate::currentDate();
    emit filterChanged();
    reload();
}

void EventLogBackend::resetFilters()
{
    from_ = QDate::currentDate().addDays(-7);
    to_ = QDate::currentDate();
    channel_ = -1;
    type_ = QStringLiteral("전체 이벤트");
    conf_ = QStringLiteral("전체");
    emit filterChanged();
    reload();
}

QString EventLogBackend::typeLabel(const QString& c)
{
    if (c == QLatin1String("FALL")) return QStringLiteral("낙상");
    if (c == QLatin1String("EGRESS")) return QStringLiteral("침상이탈");
    if (c == QLatin1String("VITAL_ABNORMAL")) return QStringLiteral("생체신호 이상");
    return c;
}
QString EventLogBackend::sourceLabel(const QString& c)
{
    if (c == QLatin1String("CAMERA")) return QStringLiteral("카메라");
    if (c == QLatin1String("WEARABLE")) return QStringLiteral("웨어러블");
    return c;
}

void EventLogBackend::recount()
{
    unconfirmed_ = 0;
    for (const auto& v : rows_)
        if (!v.toMap().value("confirmed").toBool()) ++unconfirmed_;
    countText_ = unconfirmed_ > 0 ? QStringLiteral("%1건  ·  미확인 %2건").arg(rows_.size()).arg(unconfirmed_)
                                  : QStringLiteral("%1건").arg(rows_.size());
    if (truncated_)
        countText_ += QStringLiteral("  ·  %1행까지만 표시 — 기간을 좀 더 좁혀 보세요").arg(kMaxRows);
}

void EventLogBackend::reload()
{
    rows_.clear();
    truncated_ = false;
    if (!dbOpen()) { recount(); emit rowsChanged(); return; }

    QString sql = QStringLiteral(
        "SELECT e.event_id, UNIX_TIMESTAMP(e.occurred_at)*1000, e.camera_id, "
        "       e.event_type, e.source, e.clip_url, e.confirmed_at, e.confirmed_by, "
        "       r.name, r.room "
        "FROM events e LEFT JOIN residents r ON r.resident_id = e.resident_id "
        "WHERE DATE(e.occurred_at) BETWEEN ? AND ? ");
    QVariantList binds{from_, to_};
    if (type_ == QStringLiteral("낙상")) sql += "AND e.event_type='FALL' ";
    else if (type_ == QStringLiteral("침상이탈")) sql += "AND e.event_type='EGRESS' ";
    else if (type_ == QStringLiteral("생체신호 이상")) sql += "AND e.event_type='VITAL_ABNORMAL' ";
    if (channel_ >= 0) { sql += "AND e.camera_id = ? "; binds << channel_; }
    if (conf_ == QStringLiteral("미확인만")) sql += "AND e.confirmed_at IS NULL ";
    else if (conf_ == QStringLiteral("확인만")) sql += "AND e.confirmed_at IS NOT NULL ";
    sql += QStringLiteral("ORDER BY e.occurred_at DESC LIMIT %1").arg(kMaxRows + 1);

    QSqlQuery q;
    q.prepare(sql);
    for (const auto& b : binds) q.addBindValue(b);
    if (!q.exec()) {
        qDebug() << "이벤트 원장 조회 실패:" << q.lastError().text();
        recount(); emit rowsChanged(); return;
    }
    while (q.next()) {
        if (rows_.size() >= kMaxRows) { truncated_ = true; break; }
        const int ch = q.value(2).isNull() ? -1 : q.value(2).toInt();
        const qint64 ms = q.value(1).toLongLong();
        QString clip;
        const QString file = q.value(5).toString();
        if (!file.isEmpty() && ch >= 0)
            clip = file.startsWith(QLatin1String("http")) ? file
                 : QStringLiteral("http://%1:%2/%3").arg(hostForChannel(ch)).arg(kClipHttpPort).arg(file);
        const QString room = q.value(9).toString();
        QVariantMap m;
        m["eventId"] = q.value(0).toLongLong();
        m["atMs"] = ms;
        m["when"] = QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss");
        m["channel"] = ch;
        m["type"] = typeLabel(q.value(3).toString());
        m["place"] = QStringLiteral("%1 · 채널 %2").arg(room.isEmpty() ? roomName() : room).arg(ch + 1);
        m["resident"] = q.value(8).toString().isEmpty() ? QStringLiteral("신원 미상") : q.value(8).toString();
        m["source"] = sourceLabel(q.value(4).toString());
        m["confirmed"] = !q.value(6).isNull();
        m["confirmedBy"] = q.value(7).toString();
        m["clipUrl"] = clip;
        rows_ << m;
    }
    recount();
    emit rowsChanged();
}

QVariantMap EventLogBackend::activate(int row)
{
    QVariantMap out;
    if (row < 0 || row >= rows_.size()) return out;
    QVariantMap m = rows_[row].toMap();
    out["context"] = QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
                         .arg(m["when"].toString(), m["type"].toString(), m["resident"].toString(), m["place"].toString());
    out["clipUrl"] = m["clipUrl"];
    out["channel"] = m["channel"];
    out["atMs"] = m["atMs"];

    if (!m["clipUrl"].toString().isEmpty() && !m["confirmed"].toBool()) {
        const qint64 id = m["eventId"].toLongLong();
        const QString who = app_ ? app_->userName() : QString();
        if (id > 0 && dbOpen()) {
            QSqlQuery q;
            q.prepare(QStringLiteral("UPDATE events SET confirmed_at = NOW(), confirmed_by = ? "
                                     "WHERE event_id = ? AND confirmed_at IS NULL"));
            q.addBindValue(who);
            q.addBindValue(id);
            if (!q.exec()) qDebug() << "이벤트 확인 기록 실패:" << q.lastError().text();
        }
        m["confirmed"] = true;
        m["confirmedBy"] = who;
        rows_[row] = m;
        recount();
        emit rowsChanged();
    }
    return out;
}

void EventLogBackend::appendLive(int channel, int roiId, double occurredMs, const QString& typeCode, const QString& source)
{
    const qint64 ms = qint64(occurredMs);
    QVariantMap m;
    m["eventId"] = qint64(0);
    m["atMs"] = ms;
    m["when"] = QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss");
    m["channel"] = channel;
    m["type"] = typeLabel(typeCode);
    m["place"] = dir_->placeLabel(channel, roiId);
    const QString who = dir_->zoneResidentName(channel, roiId);
    m["resident"] = who.isEmpty() ? QStringLiteral("신원 미상") : who;
    m["source"] = sourceLabel(source);
    m["confirmed"] = false;
    m["confirmedBy"] = QString();
    m["clipUrl"] = QStringLiteral("http://%1:%2/ch%3_%4_%5.mp4")
                       .arg(hostForChannel(channel)).arg(kClipHttpPort).arg(channel).arg(ms).arg(typeCode);
    rows_.prepend(m);
    recount();
    emit rowsChanged();
}

void EventLogBackend::refreshNvr()
{
    nvr_.clear();
    for (int si = 0; si < StreamClient::kNumServers; ++si) {
        const QString host = StreamClient::serverHost(si);
        QNetworkReply* reply = net_->get(QNetworkRequest(QUrl(QStringLiteral("http://%1:%2/list").arg(host).arg(kNvrHttpPort))));
        connect(reply, &QNetworkReply::finished, this, [this, reply, host] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "NVR 세그먼트 목록 수집 실패(" << host << "):" << reply->errorString();
                return;
            }
            const auto doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) return;
            for (const auto& v : doc.array()) {
                const QString fileName = v.toString();
                const QStringList parts = fileName.left(fileName.lastIndexOf('.')).split('_');
                if (parts.size() != 2) continue;
                const int ch = parts[0].mid(2).toInt();
                if (ch < 0 || ch >= 4) continue;
                nvr_.push_back({ch, parts[1].toLongLong(),
                                QStringLiteral("http://%1:%2/%3").arg(host).arg(kNvrHttpPort).arg(fileName)});
            }
            emit nvrChanged();
        });
    }
}

QVariantMap EventLogBackend::nvrContext(int channel, double timestampMs) const
{
    const qint64 ts = qint64(timestampMs);
    const NvrSeg* best = nullptr;
    for (const auto& s : nvr_) {
        if (s.channel != channel || s.startMs > ts) continue;
        if (!best || s.startMs > best->startMs) best = &s;
    }
    if (!best)
        return {{"ok", false}, {"message", QStringLiteral("이 시점에 해당하는 NVR 녹화를 찾을 수 없습니다.\n"
                 "(보존기간 경과, 미녹화 구간, 또는 아직 목록을 못 받아왔을 수 있습니다 — 새로고침 후 다시 시도해보세요.)")}};
    return {{"ok", true}, {"url", best->url}, {"seekMs", double(ts - best->startMs)}};
}

namespace { constexpr qint64 kSegLenMs = 10 * 60 * 1000; constexpr qint64 kJoinGapMs = 30 * 1000; }

QVariantList EventLogBackend::spans(int channel) const
{
    QVector<qint64> starts;
    for (const auto& s : nvr_) if (s.channel == channel) starts.push_back(s.startMs);
    std::sort(starts.begin(), starts.end());
    QVector<QPair<qint64, qint64>> merged;
    for (qint64 st : starts) {
        if (!merged.isEmpty() && st - merged.back().second <= kJoinGapMs) merged.back().second = st + kSegLenMs;
        else merged.push_back({st, st + kSegLenMs});
    }
    QVariantList out;
    for (const auto& m : merged) out.push_back(QVariantList{m.first, m.second});
    return out;
}

QVariantList EventLogBackend::markers(int channel) const
{
    QVariantList out;
    for (const QVariant& v : rows_) {
        const QVariantMap m = v.toMap();
        if (m.value("channel").toInt() != channel) continue;
        const QString t = m.value("type").toString();
        const char* c = t == QStringLiteral("낙상") ? kCritical : t == QStringLiteral("침상이탈") ? kHigh : kWarn;
        out.push_back(QVariantMap{{"atMs", m.value("atMs")}, {"color", QString::fromLatin1(c)}});
    }
    return out;
}

QVariantMap EventLogBackend::segmentAt(int channel, double ms) const
{
    const qint64 t = qint64(ms);
    const NvrSeg* best = nullptr;
    for (const auto& s : nvr_) {
        if (s.channel != channel || t < s.startMs || t >= s.startMs + kSegLenMs) continue;
        if (!best || s.startMs > best->startMs) best = &s;
    }
    if (!best) return {{"ok", false}};
    return {{"ok", true}, {"url", best->url}, {"startMs", double(best->startMs)}, {"seekMs", double(t - best->startMs)}};
}

QString EventLogBackend::suggestedName(const QString& url) const { return QUrl(url).fileName(); }

void EventLogBackend::download(const QString& url, const QString& path)
{
    QString local = path;
    if (local.startsWith(QLatin1String("file:///"))) local = QUrl(local).toLocalFile();
    QNetworkReply* reply = net_->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, local] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { emit downloadFinished(false, reply->errorString()); return; }
        QFile f(local);
        if (!f.open(QIODevice::WriteOnly)) {
            emit downloadFinished(false, QStringLiteral("파일을 저장할 수 없습니다: %1").arg(local));
            return;
        }
        f.write(reply->readAll());
        emit downloadFinished(true, QStringLiteral("저장됨: %1").arg(local));
    });
}
