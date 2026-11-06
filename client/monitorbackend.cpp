#include "monitorbackend.h"

#include "frameprovider.h"
#include "caredirectory.h"
#include "streamclient.h"
#include "theme.h"

#include <QDir>
#include <QSettings>

namespace {
// 이 시간 동안 새 프레임이 안 오면 소켓은 붙어 있어도 "신호 끊김"으로 본다
// (위젯 앱 kChannelStaleTimeoutMs와 같은 값)
constexpr qint64 kStaleMs = 5000;
// 검색 응답(Gemini+DB 왕복)이 안 오면 버튼을 다시 풀어 주는 시간
constexpr int kSearchTimeoutMs = 30000;
// 침대 번호 유효 범위 (videoview.h kMaxRoiZones와 같은 값)
constexpr int kMaxRoiZones = 8;

QString typeLabel(const QString& type)
{
    if (type == QLatin1String("FALL"))           return QStringLiteral("낙상");
    if (type == QLatin1String("EGRESS"))         return QStringLiteral("침상 이탈");
    if (type == QLatin1String("VITAL_ABNORMAL")) return QStringLiteral("생체 이상");
    return QStringLiteral("이벤트");
}
}  // namespace

// ═══════════════════════════════════════════════════════════
//  ChannelModel
// ═══════════════════════════════════════════════════════════
ChannelModel::ChannelModel(QObject* parent) : QAbstractListModel(parent), items_(4) {}

int ChannelModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : items_.size();
}

QVariant ChannelModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) return {};
    const Item& it = items_[index.row()];
    switch (role) {
    case ChannelRole:   return index.row();
    case LiveRole:      return it.live;
    case FrameIdRole:   return it.frameId;
    case AlarmRole:     return it.alarm;
    case AlarmTextRole: return it.alarmText;
    case AlertNxRole:   return it.nx;
    case AlertNyRole:   return it.ny;
    case DisplayNameRole: return it.displayName;
    case ZonesRole:     return it.zones;
    }
    return {};
}

QHash<int, QByteArray> ChannelModel::roleNames() const
{
    return {{ChannelRole, "channel"}, {LiveRole, "live"}, {FrameIdRole, "frameId"},
            {AlarmRole, "alarm"}, {AlarmTextRole, "alarmText"},
            {AlertNxRole, "alertNx"}, {AlertNyRole, "alertNy"},
            {DisplayNameRole, "displayName"}, {ZonesRole, "zones"}};
}

void ChannelModel::notify(int ch, const QVector<int>& roles)
{
    const QModelIndex i = index(ch);
    emit dataChanged(i, i, roles);
}

// ═══════════════════════════════════════════════════════════
//  EventModel
// ═══════════════════════════════════════════════════════════
int EventModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : events_.size();
}

QVariant EventModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size()) return {};
    const Event& e = events_[index.row()];
    switch (role) {
    case TimeRole:      return e.when.toString(QStringLiteral("HH:mm:ss"));
    case ChannelRole:   return e.channel;
    case TypeRole:      return e.type;
    case TypeLabelRole: return typeLabel(e.type);
    case WhoRole:       return e.who;
    }
    return {};
}

QHash<int, QByteArray> EventModel::roleNames() const
{
    return {{TimeRole, "time"}, {ChannelRole, "channel"}, {TypeRole, "type"},
            {TypeLabelRole, "typeLabel"}, {WhoRole, "who"}};
}

void EventModel::prepend(const Event& e)
{
    beginInsertRows(QModelIndex(), 0, 0);
    events_.prepend(e);
    endInsertRows();
    // 화면 목록은 최근 200건이면 충분 — 전체 기록은 DB 원장(events)에 있다
    constexpr int kMax = 200;
    if (events_.size() > kMax) {
        beginRemoveRows(QModelIndex(), kMax, events_.size() - 1);
        events_.resize(kMax);
        endRemoveRows();
    }
}

// ═══════════════════════════════════════════════════════════
//  MonitorBackend
// ═══════════════════════════════════════════════════════════
MonitorBackend::MonitorBackend(StreamClient* stream, FrameProvider* frames, CareDirectory* dir,
                               QObject* parent)
    : QObject(parent), stream_(stream), frames_(frames), dir_(dir), channels_(this), events_(this)
{
    connect(dir_, &CareDirectory::changed, this, &MonitorBackend::refreshNames);
    connect(dir_, &CareDirectory::zonesChanged, this, &MonitorBackend::refreshZones);
    refreshNames();
    for (int ch = 0; ch < 4; ++ch) refreshZones(ch);

    connect(stream_, &StreamClient::frameReceived, this, &MonitorBackend::onFrame);
    connect(stream_, &StreamClient::connectionStateChanged, this, &MonitorBackend::onConnection);
    connect(stream_, &StreamClient::fallEvent, this,
            [this](int ch, int roi, quint64 ts, float nx, float ny) {
                raiseAlarm(ch, QStringLiteral("FALL"), roi, ts, nx, ny);
            });
    connect(stream_, &StreamClient::bedEgressEvent, this,
            [this](int ch, int roi, quint64 ts, float nx, float ny) {
                raiseAlarm(ch, QStringLiteral("EGRESS"), roi, ts, nx, ny);
            });
    connect(stream_, &StreamClient::vitalAbnormalEvent, this, [this](int ch, quint64 ts) {
        raiseAlarm(ch, QStringLiteral("VITAL_ABNORMAL"), -1, ts, -1.f, -1.f);
    });
    connect(stream_, &StreamClient::searchResultReceived, this,
            [this](int, const QString& text) {
                searchTimeout_.stop();
                searching_ = false;
                emit searchingChanged();
                emit searchResult(text);
            });

    staleTimer_.setInterval(2000);
    connect(&staleTimer_, &QTimer::timeout, this, &MonitorBackend::checkStale);
    staleTimer_.start();

    searchTimeout_.setSingleShot(true);
    searchTimeout_.setInterval(kSearchTimeoutMs);
    connect(&searchTimeout_, &QTimer::timeout, this, [this] {
        searching_ = false;
        emit searchingChanged();
        emit searchResult(QStringLiteral("응답이 없습니다. 잠시 후 다시 시도해 주세요."));
    });
}

int MonitorBackend::connectedServers() const { return stream_->connectedServerCount(); }
int MonitorBackend::totalServers() const { return StreamClient::kNumServers; }

QString MonitorBackend::connectionText() const
{
    const int n = connectedServers();
    if (n == totalServers()) return QStringLiteral("영상 서버 연결됨");
    if (n > 0) return QStringLiteral("영상 서버 %1/%2 연결").arg(n).arg(totalServers());
    return QStringLiteral("영상 서버 접속 중...");
}

bool MonitorBackend::anyAlarm() const
{
    for (int ch = 0; ch < 4; ++ch)
        if (channels_.item(ch).alarm) return true;
    return false;
}

void MonitorBackend::onFrame(int ch, const QImage& image, qint64 /*serverTs*/)
{
    if (ch < 0 || ch >= 4) return;
    frames_->setFrame(ch, image);
    auto& it = channels_.item(ch);
    it.lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    ++it.frameId;
    QVector<int> roles{ChannelModel::FrameIdRole};
    if (!it.live) { it.live = true; roles << ChannelModel::LiveRole; }
    channels_.notify(ch, roles);
}

void MonitorBackend::onConnection()
{
    // 담당 Pi가 끊긴 채널은 LIVE 소등
    for (int ch = 0; ch < 4; ++ch) {
        auto& it = channels_.item(ch);
        if (it.live && !stream_->isChannelConnected(ch)) {
            it.live = false;
            channels_.notify(ch, {ChannelModel::LiveRole});
        }
    }
    emit connectionChanged();
}

void MonitorBackend::checkStale()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int ch = 0; ch < 4; ++ch) {
        auto& it = channels_.item(ch);
        if (it.live && now - it.lastFrameMs > kStaleMs) {
            it.live = false;
            channels_.notify(ch, {ChannelModel::LiveRole});
        }
    }
}

void MonitorBackend::raiseAlarm(int ch, const QString& type, int roiId, quint64 ts,
                                float nx, float ny)
{
    if (ch < 0 || ch >= 4) return;
    const bool hasRoi = roiId >= 0 && roiId < kMaxRoiZones;
    // 이벤트 목록용 — 아는 사실(채널·침대)만
    const QString who = hasRoi ? QStringLiteral("채널 %1 · 침대 %2").arg(ch + 1).arg(roiId + 1)
                               : QStringLiteral("채널 %1").arg(ch + 1);
    // 타일 배지용 — 위젯 앱 eventWhoLabel()과 같은 규칙(침대에 지정된 입소자 이름 포함)
    const QString place = dir_->whoLabel(ch, hasRoi ? roiId : -1);

    auto& it = channels_.item(ch);
    it.alarm = true;
    QString marker;
    if (type == QLatin1String("FALL")) {
        it.fall = true;
        it.alarmText = QStringLiteral("🚨 낙상 감지 · %1").arg(place);
        it.nx = nx; it.ny = ny;
        marker = QString::fromLatin1(kCritical);
    } else if (type == QLatin1String("EGRESS")) {
        it.egress = true;
        it.alarmText = QStringLiteral("⚠️ 침대 이탈 · %1").arg(place);
        const bool known = nx > 0 || ny > 0;
        it.nx = known ? nx : -1; it.ny = known ? ny : -1;
        marker = QString::fromLatin1(kHigh);
    } else {
        it.vital = true;
        it.alarmText = QStringLiteral("🚨 생체신호 이상");
        it.nx = -1; it.ny = -1;
        marker = QString::fromLatin1(kWarn);
    }
    channels_.notify(ch, {ChannelModel::AlarmRole, ChannelModel::AlarmTextRole,
                          ChannelModel::AlertNxRole, ChannelModel::AlertNyRole});

    EventModel::Event e;
    e.when = ts ? QDateTime::fromMSecsSinceEpoch(qint64(ts)) : QDateTime::currentDateTime();
    e.channel = ch;
    e.type = type;
    e.who = who;
    events_.prepend(e);
    emit eventRaised(ch, hasRoi ? roiId : -1, double(e.when.toMSecsSinceEpoch()), type);
    emit alarmChanged();
    emit alarmRaised(ch, e.when.toMSecsSinceEpoch(), marker);
}

QString MonitorBackend::alarmSummary() const
{
    // 위젯 앱 updateAlarmBanner()와 같은 문구
    QList<QPair<int, QString>> evts;
    for (int ch = 0; ch < 4; ++ch) {
        const auto& it = channels_.item(ch);
        if (it.fall)   evts.append({ch, QStringLiteral("낙상")});
        if (it.egress) evts.append({ch, QStringLiteral("침상이탈")});
        if (it.vital)  evts.append({ch, QStringLiteral("생체신호 이상")});
    }
    if (evts.isEmpty()) return {};
    if (evts.size() == 1)
        return QStringLiteral("채널 %1에서 %2 발생!").arg(evts[0].first + 1).arg(evts[0].second);
    QStringList parts;
    for (const auto& e : evts)
        parts << QStringLiteral("채널 %1 %2").arg(e.first + 1).arg(e.second);
    return parts.join(QStringLiteral("   ·   ")) + QStringLiteral("   (%1건)").arg(evts.size());
}

QString MonitorBackend::roomName() const { return CareDirectory::roomName(); }

void MonitorBackend::refreshNames()
{
    for (int ch = 0; ch < 4; ++ch) {
        channels_.item(ch).displayName = dir_->channelName(ch);
        channels_.notify(ch, {ChannelModel::DisplayNameRole});
    }
}

void MonitorBackend::refreshZones(int ch)
{
    if (ch < 0 || ch >= 4) return;
    QVariantList list;
    for (const auto& z : dir_->zones(ch)) {
        QVariantList pts;
        for (const QPointF& p : z.poly) pts << p.x() << p.y();
        list << QVariantMap{{QStringLiteral("id"), z.id},
                            {QStringLiteral("label"), dir_->zoneLabel(ch, z.id)},
                            {QStringLiteral("pts"), pts}};
    }
    channels_.item(ch).zones = list;
    channels_.notify(ch, {ChannelModel::ZonesRole});
}

void MonitorBackend::raiseExternalAlarm(int ch, const QString& type, const QString& label)
{
    if (ch < 0 || ch >= 4) return;
    auto& it = channels_.item(ch);
    if (it.alarm && it.fall) return;                 // 이미 낙상 경보 중
    it.alarm = true;
    it.fall = true;
    it.alarmText = label;
    it.nx = it.ny = -1;
    channels_.notify(ch, {ChannelModel::AlarmRole, ChannelModel::AlarmTextRole,
                          ChannelModel::AlertNxRole, ChannelModel::AlertNyRole});
    EventModel::Event e;
    e.when = QDateTime::currentDateTime();
    e.channel = ch; e.type = type; e.who = QStringLiteral("채널 %1").arg(ch + 1);
    events_.prepend(e);
    emit alarmChanged();
    emit alarmRaised(ch, e.when.toMSecsSinceEpoch(), QString::fromLatin1(kCritical));
}

void MonitorBackend::clearAlarms()
{
    for (int ch = 0; ch < 4; ++ch) {
        auto& it = channels_.item(ch);
        if (!it.alarm) continue;
        it.alarm = false;
        it.fall = it.egress = it.vital = false;
        it.alarmText.clear();
        it.nx = it.ny = -1;
        channels_.notify(ch, {ChannelModel::AlarmRole, ChannelModel::AlarmTextRole,
                              ChannelModel::AlertNxRole, ChannelModel::AlertNyRole});
        stream_->sendAlarmConfirm(ch);   // 서버 모자이크 복구
    }
    emit alarmChanged();
}

bool MonitorBackend::search(int channel, const QString& query)
{
    const QString q = query.trimmed();
    if (q.isEmpty() || searching_) return false;
    if (!stream_->sendSearchQuery(channel, q)) {
        emit searchResult(QStringLiteral("영상 서버에 연결되어 있지 않습니다."));
        return false;
    }
    searching_ = true;
    emit searchingChanged();
    searchTimeout_.start();
    return true;
}

QVariantMap MonitorBackend::parseSearch(const QString& text) const
{
    QVariantList hits;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QString::fromUtf8("· "))) {
            const QString rest = line.mid(2).trimmed();
            QVariantMap h;
            const int sep = rest.indexOf(QString::fromUtf8(" · "));
            if (sep > 0) { h["when"] = rest.left(sep); h["meta"] = rest.mid(sep + 3); }
            else { h["when"] = rest; h["meta"] = QString(); }
            h["url"] = QString();
            hits << h;
        } else if (!hits.isEmpty() && line.startsWith(QLatin1String("http")) && line.endsWith(QLatin1String(".mp4"))) {
            QVariantMap h = hits.last().toMap();
            h["url"] = line;
            hits.last() = h;
        }
    }
    for (int i = 0; i < hits.size(); ++i) {
        QVariantMap h = hits[i].toMap();
        h["context"] = h["meta"].toString().isEmpty() ? h["when"].toString()
                     : QStringLiteral("%1  ·  %2").arg(h["when"].toString(), h["meta"].toString());
        hits[i] = h;
    }
    QVariantMap out{{"hits", hits}};
    out["message"] = hits.isEmpty() ? text.trimmed() : QString();
    return out;
}

QVariant MonitorBackend::setting(const QString& key, const QVariant& fallback) const
{
    return QSettings().value(key, fallback);
}

void MonitorBackend::setSetting(const QString& key, const QVariant& value)
{
    QSettings().setValue(key, value);
}

QString MonitorBackend::snapshotPath(int channel) const
{
    return QStringLiteral("%1/CH%2_%3.png")
        .arg(QDir::homePath())
        .arg(channel + 1)
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
}
