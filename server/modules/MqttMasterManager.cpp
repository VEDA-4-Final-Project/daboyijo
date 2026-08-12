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
    if(!mqtt_client_->connectToBroker(broker_ip, port)) {
        std::cerr << "[MqttMasterManager] Broker connection failed!" << std::endl;
        return false;
    }

    mqtt_client_->setCallback([this](const::std::string& t , const std::string& p) {
        this->onMessageReceived(t,p);
    });

    mqtt_client_->startLoop();

    mqtt_client_->subscribeTopic("veda/wearable/data");
    std::printf("[MqttMasterManager] Subscribed to 'veda/wearable/data'");
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

    mqtt_client_->publishMessage("veda/alarm/control",payload);
    std::cout << "[MqttMasterManager] SentAlarm Command to Node: " << payload<< std::endl;
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
