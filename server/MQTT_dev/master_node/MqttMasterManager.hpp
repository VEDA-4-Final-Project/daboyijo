#ifndef MQTT_SERVER_MODULE_H
#define MQTT_SERVER_MODULE_H

#include <string>
#include <memory>
#include "MqttClient_veda.hpp"

class MqttMasterManager {
public:
    MqttMasterManager();
    ~MqttMasterManager();

    //Mqtt 브로커 연결 및 구독 시작
    bool init(const std::string& broker_ip, int port = 1883);

    // 판단 함수 
    bool checkFallStatus(const WearableData& data AlarmComand& out_cmd);

    // 알림노드로 제어 명령어 전송 함수 
    void sendAlarmCommand(const AlarmComand& cmd);
private:
    // 내부 수신 롤백 
    void onMessageReceived(const std::string& topic, const std::string& payload);

    std::unique_ptr<MqttClient_veda> mqtt_client_;
}







