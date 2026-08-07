#include "mqttqtmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QMqttSubscription>
#include <QTimer>
#include <QUuid>

#include <nlohmann/json.hpp>

namespace {

// 재연결 간격. QMqttClient 는 자동 재연결을 해주지 않아서 직접 돌린다.
// 관제 앱은 끊긴 걸 사람이 알아야 하므로, 조용히 붙기만 하는 것보다
// 시도 로그가 남는 편이 낫다.
constexpr int kRetryIntervalMs = 5000;

// 구조체의 timestamp 필드는 전부 Unix epoch 밀리초로 통일한다.
long long nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// 에러 코드를 사람이 읽을 문장으로. 화면 상태바에 그대로 띄울 용도라
// 숫자만 남기지 않는다.
QString errorText(QMqttClient::ClientError error)
{
    switch (error) {
    case QMqttClient::NoError:                return QStringLiteral("정상");
    case QMqttClient::InvalidProtocolVersion: return QStringLiteral("브로커가 이 MQTT 버전을 거부했습니다");
    case QMqttClient::IdRejected:             return QStringLiteral("클라이언트 ID가 거부됐습니다");
    case QMqttClient::ServerUnavailable:      return QStringLiteral("브로커에 접속할 수 없습니다");
    case QMqttClient::BadUsernameOrPassword:  return QStringLiteral("계정 정보가 올바르지 않습니다");
    case QMqttClient::NotAuthorized:          return QStringLiteral("접속 권한이 없습니다");
    case QMqttClient::TransportInvalid:       return QStringLiteral("네트워크 연결이 끊겼습니다");
    case QMqttClient::ProtocolViolation:      return QStringLiteral("프로토콜 위반이 감지됐습니다");
    default:                                  return QStringLiteral("알 수 없는 오류");
    }
}

}  // namespace

MqttQtManager::MqttQtManager(QObject* parent)
    : QObject(parent),
      m_client(new QMqttClient(this)),
      m_retryTimer(new QTimer(this))
{
    qRegisterMetaType<WearableData>("WearableData");
    qRegisterMetaType<AlarmCommand>("AlarmCommand");

    connect(m_client, &QMqttClient::stateChanged,   this, &MqttQtManager::onStateChanged);
    connect(m_client, &QMqttClient::errorChanged,   this, &MqttQtManager::onErrorChanged);
    connect(m_client, &QMqttClient::messageReceived, this, &MqttQtManager::onMessageReceived);

    m_retryTimer->setInterval(kRetryIntervalMs);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_client->state() == QMqttClient::Disconnected) {
            qInfo() << "[MQTT] 재연결 시도...";
            m_client->connectToHost();
        }
    });
}

MqttQtManager::~MqttQtManager()
{
    // 앱을 닫을 때 이런 순서로 일이 벌어진다.
    //   ~MainWindow 본문 실행  →  MainWindow 부분은 이미 파괴됨
    //   → ~QObject 가 자식들을 삭제 → 이 객체 삭제
    //     → QMqttClient 가 끊기면서 stateChanged(Disconnected) 를 쏨
    //       → onStateChanged → emit disconnected()
    //         → MainWindow::onMqttDisconnected()  ← 이미 없는 객체! 💥
    //
    // Qt 는 이걸 잡아내고 이렇게 죽는다:
    //   ASSERT: "Called object is not of the correct type
    //            (class destructor may have already run)"
    //
    // 그래서 정리를 시작하기 전에 들어오는 연결과 나가는 시그널을 모두 끊는다.
    m_userDisconnect = true;              // 남은 재연결 시도를 막는다
    if (m_retryTimer) m_retryTimer->stop();
    if (m_client) m_client->disconnect(this);  // QMqttClient → 이 객체로 오는 연결
    blockSignals(true);                        // 이 객체 → 바깥으로 나가는 시그널
}

bool MqttQtManager::init(const QString& broker_ip, int port, const QString& client_id)
{
    if (m_client->state() != QMqttClient::Disconnected) {
        qWarning() << "[MQTT] 이미 연결돼 있어 먼저 정리합니다.";
        disconnectFromBroker();
    }

    const QString id = client_id.isEmpty()
        ? QStringLiteral("qt_console_%1")
              .arg(QUuid::createUuid().toString(QUuid::Id128).left(8))
        : client_id;

    m_client->setHostname(broker_ip);
    m_client->setPort(static_cast<quint16>(port));
    m_client->setClientId(id);

    m_userDisconnect = false;
    m_client->connectToHost();
    m_retryTimer->start();   // 첫 시도가 실패해도 계속 물고 늘어진다

    qInfo() << "[MQTT] 접속 시도:" << broker_ip << ":" << port << "id =" << id;
    return true;
}

void MqttQtManager::disconnectFromBroker()
{
    m_userDisconnect = true;
    m_retryTimer->stop();
    m_client->disconnectFromHost();
    qInfo() << "[MQTT] 연결을 끊었습니다.";
}

bool MqttQtManager::isConnected() const
{
    return m_client->state() == QMqttClient::Connected;
}

bool MqttQtManager::sendAlarmCommand(const AlarmCommand& cmd, int qos)
{
    if (!isConnected()) {
        // 여기서 큐에 쌓지 않는 것은 의도다. 끊긴 동안 눌린 "경보 해제"를
        // 재연결 후에 흘려보내면, 그 사이 새로 발생한 알람을 꺼버릴 수 있다.
        // 관제사에게는 실패했다고 알리는 편이 안전하다.
        const QString reason = QStringLiteral("브로커에 연결돼 있지 않아 명령을 보내지 못했습니다");
        qWarning() << "[MQTT]" << reason;
        emit connectionError(reason);
        return false;
    }

    try {
        const nlohmann::json j = cmd;
        const std::string payload = j.dump();

        // QLatin1String 을 그대로 넘기면 사용자 정의 변환이 두 번 필요해서
        // (QLatin1String → QString → QMqttTopicName) 컴파일이 안 된다.
        const qint32 id = m_client->publish(QMqttTopicName(QString::fromLatin1(kTopicAlarm)),
                                            QByteArray::fromStdString(payload),
                                            static_cast<quint8>(qos));
        if (id == -1) {
            qWarning() << "[MQTT] 발행 실패:" << kTopicAlarm;
            return false;
        }

        qInfo() << "[MQTT] 명령 발행:" << kTopicAlarm << "QoS" << qos;
        return true;
    } catch (const std::exception& e) {
        qCritical() << "[MQTT] 명령 직렬화 실패:" << e.what();
        return false;
    }
}

bool MqttQtManager::sendAlarmClear(const QString& room, const QString& target_device, int qos)
{
    // ⚠️ 알림 노드가 아직 audio_action == "PLAY" 만 처리한다
    //    (MQTT/MQTT_dev/src/alarm_node/main_alarm.cpp). STOP 분기가 붙기 전까지는
    //    이 명령을 보내도 현장 소리가 실제로 꺼지지 않는다.
    AlarmCommand cmd{};
    cmd.target_device = target_device.toStdString();
    cmd.room          = room.toStdString();

    cmd.type          = "CONTROL";
    cmd.message       = "경보 해제";

    cmd.audio_action  = "STOP";
    cmd.audio_file    = "";
    cmd.volume        = 0;
    cmd.loop          = false;

    cmd.matrix_action = "CLEAR";
    cmd.matrix_passes = 0;
    cmd.brightness    = 0;

    cmd.timestamp     = nowMs();

    return sendAlarmCommand(cmd, qos);
}

void MqttQtManager::subscribeAll()
{
    // MQTT 는 clean session 이면 재연결할 때 브로커가 구독 목록을 잊는다.
    // 그래서 "연결될 때마다" 다시 건다. 최초 연결도 이 경로를 탄다.
    struct Sub { const char* topic; quint8 qos; };
    const Sub subs[] = {
        { kTopicWearable, 0 },   // 주기 데이터 — 하나쯤 놓쳐도 다음 게 온다
        { kTopicAlarm,    1 },   // 알람 — 놓치면 안 된다
    };

    for (const Sub& s : subs) {
        auto* sub = m_client->subscribe(QMqttTopicFilter(QString::fromLatin1(s.topic)), s.qos);
        if (!sub) {
            qWarning() << "[MQTT] 구독 실패:" << s.topic;
        } else {
            qInfo() << "[MQTT] 구독:" << s.topic << "QoS" << s.qos;
        }
    }
}

void MqttQtManager::onStateChanged(QMqttClient::ClientState state)
{
    switch (state) {
    case QMqttClient::Connected:
        m_retryTimer->stop();
        subscribeAll();
        qInfo() << "[MQTT] 연결 완료.";
        emit connected();
        break;

    case QMqttClient::Disconnected:
        emit disconnected();
        if (!m_userDisconnect) {
            // 사고로 끊긴 경우에만 다시 붙으려 시도한다.
            qWarning() << "[MQTT] 연결이 끊겼습니다. 재연결을 시도합니다.";
            m_retryTimer->start();
        }
        break;

    case QMqttClient::Connecting:
        break;
    }
}

void MqttQtManager::onErrorChanged(QMqttClient::ClientError error)
{
    if (error == QMqttClient::NoError) return;

    const QString text = errorText(error);
    qWarning() << "[MQTT] 오류:" << text;
    emit connectionError(text);
}

void MqttQtManager::onMessageReceived(const QByteArray& payload, const QMqttTopicName& topic)
{
    // QMqttClient 는 Qt 이벤트 루프 위에서 돌기 때문에 여기는 이미 메인
    // 스레드다. libmosquitto 를 직접 쓸 때 필요했던 스레드 넘기기가 없다.
    const QString topicName = topic.name();

    try {
        const auto j = nlohmann::json::parse(payload.constData(),
                                             payload.constData() + payload.size());

        if (topicName == QLatin1String(kTopicWearable)) {
            emit wearableDataReceived(j.get<WearableData>());
        } else if (topicName == QLatin1String(kTopicAlarm)) {
            emit alarmCommandReceived(j.get<AlarmCommand>());
        }
        // 구독한 적 없는 토픽은 무시한다.
    } catch (const std::exception& e) {
        // 다른 노드가 형식을 바꿨거나 필드를 빠뜨린 경우. 한 건만 버리고 계속 간다.
        emit payloadRejected(topicName, QString::fromLocal8Bit(e.what()));
    }
}
