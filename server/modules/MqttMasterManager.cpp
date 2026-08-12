#include "MqttMasterManager.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <chrono>

MqttMasterManager::MqttMasterManager() {
        mqtt_client_ = std::make_unique<MqttClient_veda>("Main_Master_Module");
}

MqttMasterManager::~MqttMasterManager() {
    if(mqtt_client_) {
        mqtt_client_->stopLoop();
    }
}

bool MqttMasterManager::init(const std::string& broker_ip, int port) {


    std::string ca_path = "../../MQTT/certs/ca.crt";
    mqtt_client_->setTlsConfig(ca_path);

    // 콜백·구독은 연결보다 먼저 걸어둔다 — 연결 직후 들어오는 메시지를 놓치지 않고,
    // 구독은 목록에 기록됐다가 on_connect 에서(재접속마다) 자동으로 다시 걸린다.
    mqtt_client_->setCallback([this](const::std::string& t , const std::string& p) {
        this->onMessageReceived(t,p);
    });
    mqtt_client_->subscribeTopic("veda/wearable/data");

    const bool connected = mqtt_client_->connectToBroker(broker_ip, port);

    // ★ 연결 실패해도 네트워크 루프는 반드시 띄운다.
    //   mosquitto 의 자동 재접속(reconnect_delay)은 이 루프 안에서만 돈다.
    //   예전처럼 여기서 return 해버리면 루프가 안 떠서 영원히 오프라인이 되고,
    //   그동안 발행한 알람은 전부 큐에만 쌓인다(사이렌은 안 울린다).
    mqtt_client_->startLoop();

    if(!connected) {
        std::cerr << "[MqttMasterManager] 브로커 최초 연결 실패 (" << broker_ip << ":" << port
                  << ") — 백그라운드에서 재접속을 계속 시도합니다." << std::endl;
        std::cerr << "[MqttMasterManager] TLS CA 경로: " << ca_path
                  << " (실행 위치 기준 상대경로 — 파일이 없으면 연결이 안 됩니다)" << std::endl;
        return false;
    }

    std::cout << "[MqttMasterManager] Subscribed to 'veda/wearable/data'" << std::endl;
    return true;
}


bool MqttMasterManager::checkFallStatus(const WearableData& data, AlarmCommand& out_cmd) {
    if(data.is_fall_detected) {
        out_cmd.target_device = "alarm_rpi_01";
        out_cmd.timestamp = data.timestamp;
        out_cmd.volume = 90;
        out_cmd.loop = true;

        
        out_cmd.message = "낙상 감지";
        out_cmd.audio_action = "PLAY";
        out_cmd.audio_file = "/home/mayoina/study_veda/daboyijo/server/MQTT_dev/build/alarm_node/fall_alert.wav";
        return true;
    }
    return false;
}


void MqttMasterManager::sendAlarmCommand(AlarmEventType event_type, int room ) {

    AlarmCommand cmd;
    cmd.room = room;
    
    if(event_type == AlarmEventType::FALL){
        cmd.type= "FALL";
        cmd.message = "room" + std::to_string(cmd.room) + " FALL";
        cmd.audio_file = "fall_alert.wav";
    }else if(event_type == AlarmEventType::EGRESS){
        cmd.type = "EGRESS";
        cmd.message = "room" + std::to_string(cmd.room) + " EGRESS";
        cmd.audio_file = "egress_alert.wav";
    }else if(event_type == AlarmEventType::VITAL_ABNORMAL){
        cmd.type = "VITAL_ABNORMAL";
        cmd.message = "room"+ std::to_string(cmd.room) + " VITAL_ABNORMAL";
        cmd.audio_file = "vital_alert.wav";
    }


    cmd.target_device = "alarm_rpi_01";
    //cmd.timestamp = data.timestamp;
    // chrono를 이용하여 시스템 타임 스탬프 대입 
    auto now = std::chrono::system_clock::now();
    cmd.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    cmd.volume = 90;
    cmd.loop = true;
    cmd.audio_action = "PLAY";

    // led 
    cmd.matrix_action = "SHOW";
    cmd.matrix_passes = 0;
    cmd.brightness = 0;
    
    nlohmann::json j = cmd;
    std::string payload = j.dump();

    // 알람은 "보냈다고 찍는 것"과 "실제로 나간 것"이 반드시 같아야 한다.
    // 브로커가 끊긴 상태면 큐에 담길 뿐 알림 노드는 아무 소리도 내지 않는다.
    const PublishResult pr = mqtt_client_->publish("veda/alarm/control", payload);
    if(pr == PublishResult::Sent) {
        std::cout << "[MqttMasterManager] SentAlarm Command to Node: " << payload << std::endl;
    } else if(pr == PublishResult::Queued) {
        std::cerr << "[MqttMasterManager] ⚠️ 알람 미전송 — 브로커 오프라인. 알림 노드는 울리지 않습니다."
                     " (재접속 시 전송 예정) payload: " << payload << std::endl;
    } else {
        std::cerr << "[MqttMasterManager] ⚠️ 알람 전송 실패. payload: " << payload << std::endl;
    }
}

void MqttMasterManager::onMessageReceived(const std::string& topic, const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        auto data = j.get<WearableData>();

        if(data.is_fall_detected){
            wearable_callback_(AlarmEventType::FALL, data.device_id);
        }
        // 체온 조건은 제거했다 — WearableData 에서 temperature 필드가 사라졌다.
        // 측정 하드웨어가 없어 항상 0 이었으므로 이 조건은 발동한 적이 없다.
        if(data.heart_rate > 180 || data.spo2 < 90){
            wearable_callback_(AlarmEventType::VITAL_ABNORMAL, data.device_id);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[MqttMasterManager] Parsing Error: " << e.what() << std::endl;
    }
}

void MqttMasterManager::setWearableCallback(WearableCallback cd){
    wearable_callback_ = cd;
}
