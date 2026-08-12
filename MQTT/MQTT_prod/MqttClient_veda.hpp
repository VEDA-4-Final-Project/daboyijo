#ifndef MQTT_CLIENT_VEDA
#define MQTT_CLIENT_VEDA


#include <mosquitto.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "ThreadSafeQueue.hpp"
#include "veda_messages.hpp"

struct MqttMessage {
    std::string topic;
    std::string payload;
};

// 발행 결과 — "브로커로 실제로 나갔는지"와 "오프라인이라 큐에 담았을 뿐인지"를
// 호출자가 구분해야 한다. 낙상 알람처럼 시간이 생명인 메시지는 Queued 를 성공으로
// 취급하면 안 된다(사이렌은 안 울렸는데 로그만 '전송 완료'가 된다).
enum class PublishResult {
    Sent,    // 브로커로 전송됨
    Queued,  // 오프라인 — 재접속 시 보내려고 큐에 보관
    Failed   // 전송 시도했으나 실패
};



class MqttClient_veda {
public:
    // 롤백 함수 타입 정의 
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    MqttClient_veda(const std::string& id);
    ~MqttClient_veda();
    void initTest();

    //브로커 연결, 연결 해제 tls가 추가 되었기에 기본 포트를 8883으로 수정 기존 mqtt는 188
    bool connectToBroker(const std::string& host, int port = 8883);
    void disconnect();

    // Mqtt 루프 
    void startLoop();
    void stopLoop();

    // Publish, Subscribe
    // publish()  : 전송/큐적재/실패를 구분해 돌려준다(권장).
    // publishMessage() : 기존 호출부 호환용. "실제로 나갔을 때만" true —
    //                    예전엔 큐에 담기만 해도 true 라 호출부가 오해했다.
    PublishResult publish(const std::string& topic, const std::string& payload, int qos = 0);
    bool publishMessage(const std::string& topic, const std::string& payload, int qos = 0);
    // 구독 토픽은 기억해뒀다가 재접속할 때마다 자동으로 다시 건다.
    // (clean session 이라 끊기면 브로커 쪽 구독이 사라진다 — 안 걸어주면
    //  재접속 후 조용히 아무 메시지도 안 들어온다)
    bool subscribeTopic(const std::string& topic, int qos = 0);

    //콜백 함수 등록 메소드
    void setCallback(MessageCallback callbacks);

    // TLS 설정 함수 추가 
    void setTlsConfig(const std::string& ca_path);

private:
    // 네트워크 루프 스레드(on_connect/on_disconnect)와 발행 스레드가 같이 본다.
    std::atomic<bool> m_isConnected;
    ThreadSafeQueue<MqttMessage> m_offlineQueue;

    // 재접속 시 다시 걸어줄 구독 목록 (topic, qos)
    std::vector<std::pair<std::string, int>> m_subscriptions;
    mutable std::mutex m_subMutex;
    bool sendSubscribe(const std::string& topic, int qos);   // 기록 없이 실제 구독만

    static void on_connect(struct mosquitto *mosq, void *obj, int rc);
    static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message);
    static void on_disconnect(struct mosquitto *mosq, void *obj, int rc);

    struct mosquitto *m_mosq;
    std::string m_id;
    MessageCallback m_userCallback;
    
    //TLS(openssl) 관련 멤버 변수 추가 
    bool m_use_tls = false;
    std::string m_ca_path;

};


#endif //MQTT_CLIENT_VEDA
