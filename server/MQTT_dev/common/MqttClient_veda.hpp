#ifndef MQTT_CLIENT_VEDA
#define MQTT_CLIENT_VEDA

#include <string>
#include <mosquitto.h>
#include <functional>
#include <ThreadSafeQueue.hpp>
#include <nlohmann/json.hpp>


struct MqttMessage {
    std::string topic;
    std::string payload;
};

struct WearableData {
    std::string device_id;
    bool is_fall_detected;
    double temperature;
    int heart_rate;
    long long timestamp;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WearableData, device_id, is_fall_detected, temperature,heart_rate, timestamp)

struct AlarmCommand {
    std::string target_device;
    std::string status;
    std::string message;
     
    std::string audio_action;
    std::string audio_file;
    int volume;
    bool loop;

    long long timestamp;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlarmCommand, target_device, status, message, audio_action, audio_file, volume, loop, timestamp)





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
    bool m_isConnected;
    ThreadSafeQueue<MqttMessage> m_offlineQueue;

    static void on_connect(struct mosquitto *mosq, void *obj, int rc);
    static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message);
    static void on_disconnect(struct mosquitto *mosq, void *obj, int rc);

    struct mosquitto *m_mosq;
    std::string m_id;
    MessageCallback m_userCallback;

};


#endif //MQTT_CLIENT_VEDA
