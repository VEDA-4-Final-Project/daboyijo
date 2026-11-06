#include "vitalsbackend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSettings>

#include "caredirectory.h"
#include "monitorbackend.h"
#include "mqttqtmanager.h"
#include "theme.h"
#include "vitalrules.h"

QVariant VitalModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= cards_.size()) return {};
    const Card& c = cards_[index.row()];
    switch (role) {
    case NameRole: return c.name;
    case BedRole: return c.bed;
    case Spo2Role: return c.spo2;
    case HrRole: return c.hr;
    case SeverityRole: return c.severity;
    case BadgeRole: return c.badge;
    case StaleRole: return c.stale;
    case HistoryRole: return c.history;
    case ColorRole: return c.color;
    }
    return {};
}

QHash<int, QByteArray> VitalModel::roleNames() const
{
    return {{NameRole, "name"}, {BedRole, "bed"}, {Spo2Role, "spo2"}, {HrRole, "hr"},
            {SeverityRole, "severity"}, {BadgeRole, "badge"}, {StaleRole, "stale"},
            {HistoryRole, "history"}, {ColorRole, "lineColor"}};
}

namespace {
QString sevColor(const QString& s)
{
    if (s == QLatin1String("critical")) return QString::fromLatin1(kCritical);
    if (s == QLatin1String("medium"))   return QString::fromLatin1(kWarn);
    if (s == QLatin1String("normal"))   return QString::fromLatin1(kNormal);
    return QString::fromLatin1(kTextSub);
}
}  // namespace

VitalsBackend::VitalsBackend(CareDirectory* dir, MonitorBackend* monitor, QObject* parent)
    : QObject(parent), dir_(dir), monitor_(monitor), mqtt_(new MqttQtManager(this)), model_(this)
{
    connect(dir_, &CareDirectory::changed, this, &VitalsBackend::rebuildCards);
    connect(mqtt_, &MqttQtManager::wearableDataReceived, this, &VitalsBackend::onWearable);
    connect(mqtt_, &MqttQtManager::connected, this, [this] {
        qInfo() << "[MQTT] 브로커 연결됨";
        emit brokerChanged();
    });
    connect(mqtt_, &MqttQtManager::disconnected, this, [this] {
        qWarning() << "[MQTT] 브로커 연결 끊김";   // 값이 끊긴 건 30초 뒤 "신호 끊김"으로 알려준다
        emit brokerChanged();
    });
    connect(mqtt_, &MqttQtManager::connectionError, this,
            [](const QString& m) { qWarning() << "[MQTT]" << m; });
    connect(mqtt_, &MqttQtManager::nodeOnlineChanged, this, &VitalsBackend::nodeOnlineChanged);
    connect(mqtt_, &MqttQtManager::payloadRejected, this,
            [](const QString& t, const QString& why) { qWarning() << "[MQTT] 형식이 맞지 않는 메시지 무시:" << t << why; });

    tick_.setInterval(2000);   // 값이 오래되면 "신호 끊김"으로 되돌린다
    connect(&tick_, &QTimer::timeout, this, &VitalsBackend::refreshCards);
    tick_.start();
    rebuildCards();
}

bool VitalsBackend::brokerConnected() const { return mqtt_->isConnected(); }

void VitalsBackend::start()
{
    QSettings s;
    const QString host = s.value(QStringLiteral("mqtt/brokerHost"), QStringLiteral("172.20.32.79")).toString();
    int port = s.value(QStringLiteral("mqtt/brokerPort"), 8883).toInt();
    const QString ca = s.value(QStringLiteral("mqtt/caCert"),
                               QCoreApplication::applicationDirPath() + QStringLiteral("/certs/ca.crt")).toString();
    if (QFile::exists(ca)) {
        mqtt_->setTlsConfig(ca);
    } else {
        // 평문으로 내려갈 때는 포트도 같이 내려야 한다(8883은 TLS 전용)
        if (!s.contains(QStringLiteral("mqtt/brokerPort"))) port = 1883;
        qWarning() << "[MQTT] CA 인증서가 없어 평문(" << port << ")으로 접속합니다:" << ca;
    }
    mqtt_->init(host, port);
}

void VitalsBackend::rebuildCards()
{
    // 입소자가 없는 채널은 "미배정" 카드 한 장 — 위젯 rebuildVitalCards()와 같은 규칙
    QVector<VitalModel::Card> list;
    const QString room = CareDirectory::roomName();
    for (int ch = 0; ch < 4; ++ch) {
        const QString bed = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
        const auto& ids = dir_->residentsOn(ch);
        if (ids.isEmpty()) {
            VitalModel::Card c;
            c.key = -(ch + 1); c.name = QStringLiteral("미배정"); c.bed = bed;
            list.append(c);
            continue;
        }
        for (int rid : ids) {
            VitalModel::Card c;
            c.key = rid; c.name = dir_->resident(rid).name; c.bed = bed;
            list.append(c);
        }
    }
    // 목록에서 빠진 입소자의 값·이력은 버린다(재입원 때 몇 달 전 그래프가 되살아나지 않게)
    for (auto it = samples_.begin(); it != samples_.end();)
        it = dir_->resident(it.key()).channel < 0 ? samples_.erase(it) : ++it;
    for (auto it = history_.begin(); it != history_.end();)
        it = dir_->resident(it.key()).channel < 0 ? history_.erase(it) : ++it;
    model_.reset(list);
    refreshCards();
}

void VitalsBackend::refreshCards()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto& cards = model_.cards();
    for (int i = 0; i < cards.size(); ++i) {
        auto& c = cards[i];
        const Sample v = samples_.value(c.key);
        const bool fresh = v.received && (now - v.at) <= VitalRules::kStaleMs;
        const bool worn = VitalRules::worn(v.spo2, v.hr);
        QVariantList hist;
        for (double d : history_.value(c.key)) hist << d;
        c.history = hist;
        if (!fresh || !worn) {
            const QString label = !fresh ? (v.received ? QStringLiteral("신호 끊김") : QStringLiteral("대기"))
                                         : QStringLiteral("미착용");
            c.spo2 = c.hr = QStringLiteral("--");
            c.severity.clear();
            c.stale = true;
            c.badge = VitalRules::glyph({}, true) + QStringLiteral(" ") + label;
        } else {
            c.severity = VitalRules::severity(v.spo2, v.hr);
            c.stale = false;
            c.spo2 = v.spo2 > 0 ? QString::number(v.spo2) : QStringLiteral("--");
            c.hr = QString::number(v.hr);
            c.badge = VitalRules::glyph(c.severity) + QStringLiteral(" ") + VitalRules::statusLabel(v.spo2, v.hr);
        }
        c.color = sevColor(c.severity);
        model_.touch(i);
    }
}

void VitalsBackend::onWearable(const WearableData& d)
{
    const QString id = QString::fromStdString(d.device_id).trimmed();
    const int rid = dir_->residentForWearable(id);
    if (rid < 0) {
        qDebug() << "[MQTT] 미등록 웨어러블 무시:" << id << "(residents.wearable_id 에 등록하면 표시됩니다)";
        return;
    }
    Sample& v = samples_[rid];
    v.received = true; v.hr = d.heart_rate; v.spo2 = d.spo2;
    v.at = QDateTime::currentMSecsSinceEpoch();
    auto& h = history_[rid];
    h.append(d.heart_rate);
    while (h.size() > VitalRules::kHistoryMax) h.removeFirst();

    const auto info = dir_->resident(rid);
    if (d.is_fall_detected && info.channel >= 0 && info.channel < 4)
        monitor_->raiseExternalAlarm(info.channel, QStringLiteral("FALL"),
                                     QStringLiteral("🚨 %1 낙상 감지").arg(info.name));
    refreshCards();   // 도착 즉시 반영 (2초 타이머를 기다리지 않는다)
}
