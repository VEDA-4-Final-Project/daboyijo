#ifndef MQTT_CLIENT_VEDA
#define MQTT_CLIENT_VEDA

#include <string>
#include <mosquitto.h>
#include <functional>

class MqttClient_veda {
public:
    // 롤백 함수 타입 정의 
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    MqttClient_veda(const std::string& id);
    ~MqttClient_veda();
    void initTest();

    //브로커 연결, 연결 해제
    bool connectToBroker(const std::string& host, int port = 1883);
    void disconnect();

    // Mqtt 루프 
    void startLoop();
    void stopLoop();

    // Publish, Subscribe
    bool publishMessage(const std::string& topic, const std::string& payload, int qos = 0);
    bool subscribeTopic(const std::string& topic, int qos = 0);

    //콜백 함수 등록 메소드
    void setCallback(MessageCallback callbacks);
private:
    static void on_connect(struct mosquitto *mosq, void *obj, int rc);
    static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message);

    struct mosquitto *m_mosq;
    std::string m_id;
    MessageCallback m_userCallback;

};


#endif //MQTT_CLIENT_VEDA
