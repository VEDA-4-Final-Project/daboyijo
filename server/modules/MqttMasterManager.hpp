#ifndef MQTT_SERVER_MODULE_H
#define MQTT_SERVER_MODULE_H

#include <string>
#include <memory>
#include <functional>
#include <map>
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

    // ── 경보 래치 (웨어러블 1대당 1개) ─────────────────────────────
    // 생체신호 이상은 "사건"이 아니라 "상태"다. 이상인 동안 매 패킷마다 알람을
    // 내면 관제사가 해제해도 1초 뒤 다시 울린다 — 해제가 불가능해진다.
    // 그래서 정상→이상으로 '바뀌는 순간'에만 알리고, 정상으로 확실히 돌아올
    // 때까지 래치를 물고 있는다.
    struct AlarmLatch {
        bool vital_alarming = false;   // 생체 이상 알람을 이미 낸 상태
        bool fall_alarming  = false;   // 낙상 플래그 엣지 판정용
        int  normal_streak  = 0;       // 정상값 연속 횟수(복귀 확정용)
    };
    // 접근은 mosquitto 네트워크 스레드(onMessageReceived) 한 곳뿐이라 잠금 불필요.
    std::map<std::string, AlarmLatch> latches_;   // device_id → 래치

    std::unique_ptr<MqttClient_veda> mqtt_client_;
};




#endif //MQTT_SERVER_MODULE_H


