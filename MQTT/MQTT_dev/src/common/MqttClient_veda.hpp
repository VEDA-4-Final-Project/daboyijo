#ifndef MQTT_CLIENT_VEDA
#define MQTT_CLIENT_VEDA


#include <mosquitto.h>
#include <functional>
#include "ThreadSafeQueue.hpp"
#include "veda_messages.hpp"

struct MqttMessage {
    std::string topic;
    std::string payload;
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
    bool publishMessage(const std::string& topic, const std::string& payload, int qos = 0);
    bool subscribeTopic(const std::string& topic, int qos = 0);

    //콜백 함수 등록 메소드
    void setCallback(MessageCallback callbacks);

    // TLS 설정 함수 추가 
    void setTlsConfig(const std::string& ca_path);

private:
    bool m_isConnected;
    ThreadSafeQueue<MqttMessage> m_offlineQueue;

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
