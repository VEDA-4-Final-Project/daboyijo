#ifndef MQTTQTMANAGER_H
#define MQTTQTMANAGER_H

#include <QMqttClient>
#include <QObject>
#include <QString>

#include "veda_messages.hpp"   // WearableData / AlarmCommand (노드들과 공유하는 JSON 규격)

class QTimer;

// 관제 앱과 MQTT 브로커 사이의 다리 (README의 "알림 노드 원격 제어(MQTT 해제 신호)").
//
// 이 앱은 브로커에 대해 받는 쪽과 보내는 쪽을 동시에 한다.
//   받기   : veda/wearable/data — 중계 노드가 올린 생체·낙상 원본
//            veda/alarm/control — 중앙 노드가 알림 노드로 내린 명령
//   보내기 : veda/alarm/control — 관제사가 누른 경보 해제 등 CONTROL 명령
//
// alarm/control 을 보내면서 동시에 구독하는 게 이상해 보이지만, 그래야 중앙
// 노드가 자동으로 울린 알람도 화면에 뜬다. 대신 내가 보낸 명령이 나에게도
// 되돌아오므로, 이벤트 로그에 중복으로 쌓기 싫으면 받는 쪽에서 걸러야 한다.
//
// 낙상은 카메라 경로(TCP 0xDB4D)로도 들어온다. 같은 사건이 화면에 두 번 뜨지
// 않게 합치는 일은 이 클래스가 아니라 MainWindow 몫이다 — 여기서는 받은 것을
// 그대로 올려보낸다.
//
// ── 왜 libmosquitto 가 아니라 Qt MQTT 인가 ──────────────────
// 라즈베리파이 노드들은 libmosquitto 를 쓰지만 이 앱은 윈도우에서 돈다.
// 윈도우에서 mosquitto 는 lib/dll 을 각자 챙겨야 하고 windeployqt 도
// 안 도와준다. QMqttClient 는 내부가 QTcpSocket 이라 Qt 이벤트 루프 위에서
// 돌아서, 별도 스레드도 없고 콜백이 곧바로 메인 스레드로 온다.
// 라이브러리는 달라도 주고받는 JSON 은 veda_messages.hpp 로 똑같이 맞춘다.
class MqttQtManager : public QObject
{
    Q_OBJECT

public:
    // 토픽이 다른 노드와 한 글자라도 어긋나면 에러 없이 조용히 아무 일도
    // 안 일어난다. 그래서 여기 한 곳에서만 정의하고 나머지는 이 상수를 쓴다.
    //
    // protocol/protocol.h 에 dbj/alert/... 규격이 따로 있지만 현재 아무 모듈도
    // 쓰지 않는다. 나중에 그쪽으로 통일한다면 이 두 줄만 고치면 된다.
    static constexpr const char* kTopicWearable = "veda/wearable/data";
    static constexpr const char* kTopicAlarm    = "veda/alarm/control";

    // "테스트" 버튼이 노드 LED 에 띄우는 문구. 관제 앱의 미리보기도 같은 값을 써서
    // "미리보기 == 테스트 때 실제로 보이는 화면" 이 되도록 한 곳에서만 정의한다.
    // 글꼴은 한글 완성형 전체와 ASCII 를 담고 있으니 문구는 자유롭게 바꿔도 된다
    // (예전에는 손으로 고른 51 자뿐이라 여기 쓸 수 있는 글자가 제한됐다).
    static constexpr const char* kAlertTestText = "1234 확인";

    // 알림 노드 온라인 상태 — 노드가 veda/alarm/<node_id>/status 에 "online"/"offline"
    // 을 retain 으로 올린다(Last-Will 포함). +는 노드 id 자리 와일드카드.
    static constexpr const char* kTopicAlarmStatusFilter = "veda/alarm/+/status";

    explicit MqttQtManager(QObject* parent = nullptr);

    // 소멸 중에 나가는 시그널을 막는다. 이게 없으면 앱을 닫을 때 죽는다 —
    // 자세한 이유는 .cpp 의 소멸자 주석 참고.
    ~MqttQtManager() override;

    // MQTTS(TLS) 설정. ca.crt 경로를 지정하면 그 다음 init() 부터 암호화 접속을
    // 한다(기본 포트 8883). 비워두면 평문(1883)으로 붙는다.
    //
    // 라즈베리파이 노드의 MqttClient_veda::setTlsConfig 와 같은 역할이지만,
    // 안쪽 구현은 다르다 — mosquitto 는 CA 파일 경로만 넘기면 되는데 Qt 는
    // QSslConfiguration 객체를 만들어 connectToHostEncrypted 에 넘겨야 한다.
    void setTlsConfig(const QString& ca_path);

    // 브로커 접속을 시작한다. 접속은 비동기라서 반환값은 "요청을 걸었다"는
    // 뜻일 뿐이다. 실제로 쓸 수 있게 되는 시점은 connected() 시그널이고,
    // 실패는 connectionError() 로 온다.
    //
    // client_id 를 비우면 겹치지 않는 값을 자동으로 만든다. 같은 id 두 개가 한
    // 브로커에 붙으면 나중에 온 쪽이 먼저 있던 쪽을 끊어버려서, 관제 PC 를 두 대
    // 띄웠을 때 서로를 밀어내는 사고가 난다.
    bool init(const QString& broker_ip, int port = 8883,
              const QString& client_id = QString());

    // 사용자가 의도적으로 끊는다. 이 경우에는 자동 재연결을 멈춘다.
    void disconnectFromBroker();

    bool isConnected() const;

public slots:
    // AlarmCommand 를 JSON 으로 만들어 veda/alarm/control 에 발행한다.
    // 알람은 놓치면 안 되므로 기본 QoS 가 1(최소 1회 도착)이다. 대신 중복
    // 도착이 가능하니, 받는 노드가 같은 알람을 두 번 처리하지 않아야 한다.
    bool sendAlarmCommand(const AlarmCommand& cmd, int qos = 1);

    // "경보 해제" 버튼용. 소리를 끄고 LED 를 지우는 CONTROL 명령을 만들어 보낸다.
    bool sendAlarmClear(const QString& room,
                        const QString& target_device = QStringLiteral("alarm_rpi_01"),
                        int qos = 1);

    // 관제 앱의 "적용" — 밝기/음량을 스크롤·소리 없이 즉시 바꾼다. 노드가 이 값을 평상시(base)
    // 로 커밋하므로 idle "감시 중" 이 바로 이 밝기가 되고 유지된다. 이후 이벤트가 0(유지)을
    // 보내도 이 값이 쓰인다. (앱은 이 값을 QSettings 에 저장) 0 은 "유지" 라 0 을 보내면 무변화.
    bool sendAlarmConfig(const QString& target_device,
                         int brightness255, int volumePct, int qos = 1);

    // 관제 앱의 "테스트" — 현재 값으로 잠깐 보여만 준다(저장·유지 안 함). 노드가 이 값으로
    // 폰트 내 문구를 1회 스크롤 + 소리를 내고, 끝나면 평상시(base) 밝기·음량으로 되돌린다.
    bool sendAlarmTest(const QString& target_device,
                       int brightness255, int volumePct, int qos = 1);

    // 원격 방송(인터콤) 토글. on=true 면 노드가 사이렌/WAV 를 즉시 멈추고 마이크만
    // 내보낸다(audio_action=MIC_ON), false 면 마이크를 놓아 WAV 재생이 다시 가능한
    // 상태로 되돌린다(MIC_OFF). matrix 는 안 건드린다 — 방송 중에도 경보 화면은 그대로.
    bool sendBroadcastToggle(const QString& target_device, bool on, int qos = 1);

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& errorMsg);

    void wearableDataReceived(const WearableData& data);
    void alarmCommandReceived(const AlarmCommand& cmd);

    // 알림 노드 온라인/오프라인 — kTopicAlarmStatusFilter 로 들어온 retain 상태.
    void nodeOnlineChanged(const QString& node, bool online);

    // JSON 이 깨졌거나 필드가 빠진 경우. 버리기는 하되 조용히 버리지는 않는다.
    void payloadRejected(const QString& topic, const QString& reason);

private slots:
    void onStateChanged(QMqttClient::ClientState state);
    void onErrorChanged(QMqttClient::ClientError error);
    void onMessageReceived(const QByteArray& payload, const QMqttTopicName& topic);

private:
    void subscribeAll();

    // 접속을 실제로 거는 곳. TLS 설정 여부에 따라 평문/암호화를 고른다.
    // 최초 접속과 재연결 타이머가 같이 쓴다 — 한쪽만 고치면 "처음엔 붙는데
    // 끊겼다 붙을 때 실패" 하는 고약한 버그가 된다.
    void startConnection();

    QMqttClient* m_client = nullptr;
    QTimer*      m_retryTimer = nullptr;      // QMqttClient 에는 자동 재연결이 없다
    bool         m_userDisconnect = false;    // 우리가 끊은 것인지, 사고인지 구분
    QString      m_caPath;                    // 비어 있으면 TLS 미사용(평문)
};

// 지금은 시그널이 전부 메인 스레드에서 나가지만, 나중에 누가 큐 연결
// (Qt::QueuedConnection)로 붙일 수 있으니 미리 등록해둔다.
Q_DECLARE_METATYPE(WearableData)
Q_DECLARE_METATYPE(AlarmCommand)

#endif  // MQTTQTMANAGER_H
