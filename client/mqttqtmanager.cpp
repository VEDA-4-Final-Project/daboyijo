#include "mqttqtmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QMqttSubscription>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>

#include <nlohmann/json.hpp>

namespace {

// 재연결 간격. QMqttClient 는 자동 재연결을 해주지 않아서 직접 돌린다.
// 관제 앱은 끊긴 걸 사람이 알아야 하므로, 조용히 붙기만 하는 것보다
// 시도 로그가 남는 편이 낫다.
constexpr int kRetryIntervalMs = 5000;

// 브로커 인증서에 적힌 이름(CN). MQTT/scripts/generate_certs.sh 의
//   openssl req -new -key server.key ... -subj "/CN=DaboBroker"
// 와 반드시 같아야 한다. 스크립트에서 이름을 바꾸면 여기도 바꾼다.
constexpr const char* kBrokerCommonName = "DaboBroker";

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
            startConnection();   // 최초 접속과 같은 경로 — TLS 설정도 그대로 적용된다
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
    startConnection();
    m_retryTimer->start();   // 첫 시도가 실패해도 계속 물고 늘어진다

    qInfo() << "[MQTT] 접속 시도:" << broker_ip << ":" << port
            << (m_caPath.isEmpty() ? "(평문)" : "(MQTTS)") << "id =" << id;
    return true;
}

void MqttQtManager::setTlsConfig(const QString& ca_path)
{
    m_caPath = ca_path;
}

void MqttQtManager::startConnection()
{
    if (m_caPath.isEmpty()) {
        m_client->connectToHost();          // 평문 MQTT (1883)
        return;
    }

    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();

    // 우리 브로커 인증서는 공인 CA 가 아니라 자체 CA(DavoCA)가 서명했다.
    // 그 CA 를 신뢰 목록에 넣어주지 않으면 "서명한 곳을 모르겠다"며 끊긴다.
    const QList<QSslCertificate> ca = QSslCertificate::fromPath(m_caPath);
    if (ca.isEmpty()) {
        // 여기서 멈추지 않고 진행하면 원인을 알 수 없는 핸드셰이크 실패가 되므로
        // 경로 문제라는 걸 분명히 남긴다.
        const QString reason =
            QStringLiteral("CA 인증서를 읽지 못했습니다: %1").arg(m_caPath);
        qWarning() << "[MQTT]" << reason;
        emit connectionError(reason);
        return;
    }
    conf.setCaCertificates(ca);
    conf.setPeerVerifyMode(QSslSocket::VerifyPeer);   // 도장(CA 서명) 검증은 그대로 켠다

    m_client->connectToHostEncrypted(conf);   // MQTTS (8883)

    // 브로커 인증서에는 이름(CN=DaboBroker)만 있고 IP 는 안 적혀 있는데(SAN 확장
    // 자체가 없다), 우리는 IP 로 접속한다. 그래서 Qt 가 "인증서 이름과 접속 주소가
    // 다르다"(HostNameMismatch)며 핸드셰이크를 끊는다.
    //
    // QSslConfiguration 만으로는 "이름 검증만 빼고 나머지는 검증"을 고를 수 없다
    // (setPeerVerifyMode 는 전부 켜거나 전부 끄는 것뿐). 그래서 방금
    // connectToHostEncrypted() 가 내부에 만든 소켓을 붙잡아, 이 에러 하나만
    // 직접 판단한다.
    //
    // 중요한 건 그냥 무시하지 않는다는 점이다. 인증서에 적힌 CN 을 읽어
    // kBrokerCommonName 과 대조해서, 맞을 때만 넘어간다 — 즉 주소 대신 이름으로
    // 신원을 확인한다. 이렇게 하면 같은 CA 가 발급한 다른 장비 인증서
    // (예: CN=wearable_01)가 브로커 행세를 하는 것도 막힌다.
    //
    // 서명이 가짜거나 기간이 만료된 경우 등 나머지 에러는 목록에 넣지 않으므로
    // 그대로 연결이 끊긴다.
    if (auto* socket = qobject_cast<QSslSocket*>(m_client->transport())) {
        connect(socket, &QSslSocket::sslErrors, this,
                [socket](const QList<QSslError>& errors) {
                    QList<QSslError> ignorable;
                    for (const QSslError& e : errors) {
                        if (e.error() != QSslError::HostNameMismatch) continue;

                        const QStringList cn =
                            e.certificate().subjectInfo(QSslCertificate::CommonName);
                        if (cn.contains(QLatin1String(kBrokerCommonName))) {
                            ignorable.append(e);
                        } else {
                            qWarning() << "[MQTT] 인증서 이름이 다릅니다. 기대:"
                                       << kBrokerCommonName << "실제:" << cn;
                        }
                    }
                    // 목록이 비면 아무것도 무시하지 않는다 = 원래대로 연결 실패.
                    if (!ignorable.isEmpty()) socket->ignoreSslErrors(ignorable);
                });
    } else {
        qWarning() << "[MQTT] 내부 SSL 소켓을 찾지 못했습니다. TLS 접속이 실패할 수 있습니다.";
    }
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

    // AlarmCommand::room 은 정수인데(veda_messages.hpp) DB 의 residents.room 은
    // varchar 다. 여기가 그 경계라서 여기서 변환한다.
    // "302호", "302 호" 처럼 숫자 뒤에 글자가 붙은 표기도 들어오므로 숫자만 뽑는다.
    bool ok = false;
    int roomNo = room.toInt(&ok);
    if (!ok) {
        QString digits;
        for (const QChar c : room) {
            if (c.isDigit()) digits.append(c);
            else if (!digits.isEmpty()) break;   // 앞쪽 숫자 덩어리까지만
        }
        roomNo = digits.toInt(&ok);
    }
    if (!ok) {
        // 0 을 보내면 알림 노드가 엉뚱한 호실을 띄운다. 조용히 넘기지 않는다.
        const QString reason =
            QStringLiteral("호실을 숫자로 읽을 수 없어 경보 해제를 보내지 못했습니다: \"%1\"")
                .arg(room);
        qWarning() << "[MQTT]" << reason;
        emit connectionError(reason);
        return false;
    }
    cmd.room = roomNo;

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

bool MqttQtManager::sendAlarmConfig(const QString& target_device,
                                    int brightness255, int volumePct, int qos)
{
    AlarmCommand cmd{};
    cmd.target_device = target_device.toStdString();
    cmd.room          = 0;

    cmd.type          = "CONTROL";
    cmd.message       = "설정 적용";

    // matrix/audio 를 NONE 으로 둔 "순수 적용" — 스크롤·소리 없이 밝기/음량만 바로 바꾼다.
    // 노드는 이 값을 평상시(base)로 커밋하므로 idle "감시 중" 이 즉시 이 밝기로 바뀌고 유지된다.
    cmd.audio_action  = "NONE";
    cmd.audio_file    = "";
    cmd.volume        = volumePct;       // 0~100. 노드가 >0 이면 믹서에 적용·유지
    cmd.loop          = false;

    cmd.matrix_action = "NONE";
    cmd.matrix_passes = 0;
    cmd.brightness    = brightness255;   // 0~255. 노드가 >0 이면 sysfs 전역값에 적용·유지

    cmd.timestamp     = nowMs();

    return sendAlarmCommand(cmd, qos);
}

bool MqttQtManager::sendAlarmTest(const QString& target_device,
                                  int brightness255, int volumePct, int qos)
{
    AlarmCommand cmd{};
    cmd.target_device = target_device.toStdString();
    cmd.room          = 0;               // 0 = 노드가 message 를 그대로 스크롤

    cmd.type          = "CONTROL";
    // 미리보기(AlertMatrixPreview)와 같은 문구를 쓰도록 상수로 한 곳에서 정의한다.
    // ("알림 테스트"는 폰트에 글자가 없어 예전엔 빈칸으로 스크롤돼 안 보였다.)
    cmd.message       = kAlertTestText;

    cmd.audio_action  = "PLAY";
    cmd.audio_file    = "fall_alert.wav";  // 노드 sounds/ 에 있는 파일
    cmd.volume        = volumePct;
    cmd.loop          = false;

    cmd.matrix_action = "SHOW";
    cmd.matrix_passes = 1;
    cmd.brightness    = brightness255;

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
