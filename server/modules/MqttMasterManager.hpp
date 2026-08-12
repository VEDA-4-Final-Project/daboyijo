#ifndef MQTT_SERVER_MODULE_H
#define MQTT_SERVER_MODULE_H

#include <string>
#include <memory>
#include <functional>
#include "MqttClient_veda.hpp"

enum class AlarmEventType {
    FALL,
    EGRESS,
    VITAL_ABNORMAL
};

class MqttMasterManager {
public:
    MqttMasterManager();
    ~MqttMasterManager();

    //Mqtt 브로커 연결 및 구독 시작
    bool init(const std::string& broker_ip, int port = 1883);

    // 판단 함수(아마 안쓰게 될 가능성이 큽니다.)
    bool checkFallStatus(const WearableData& data, AlarmCommand& out_cmd);

    // 알림노드로 제어 명령어 전송 함수 
    void sendAlarmCommand(AlarmEventType event_type, int room);

    // 함수 타입 정의 (c에서의 typedef)
    // 이벤트 종류 + 어느 웨어러블에서 왔는지(device_id) 를 함께 넘긴다.
    // device_id 로 residents.wearable_id 를 조회해 채널/호실을 얻는다.
    using WearableCallback = std::function<void(AlarmEventType, std::string)>;

    // 함수 정의 하는 함수 
    void setWearableCallback(WearableCallback cb);


private:
    // 내부 수신 콜백 
    void onMessageReceived(const std::string& topic, const std::string& payload);

    WearableCallback wearable_callback_;


    std::unique_ptr<MqttClient_veda> mqtt_client_;
};




#endif //MQTT_SERVER_MODULE_H


