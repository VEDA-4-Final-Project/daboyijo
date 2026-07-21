#include "MqttMasterManager.hpp"
#include <iostream>
#include <nlohmann/json.hpp>

MqttMasterManager::MqttMasterManager() {
        mqtt_client_ = std::make_unique<MqttClient_veda>("Main_Master_Module");
}

MqttMasterManager::~MqttMasterManager() {
    if(mqtt_client_) {
        mqtt_client_->stopLoop();
    }
}

bool MqttMasterManager::init(const std::string& broker_ip, int port) {
    if(!mqtt_client_->connectToBroker(broker_ip, port)) {
        std::cerr << "[MqttMasterManager] Broker connection failed!" << std::endl;
        return false;
    }

    mqtt_client_->setCallback([this](const::std::string& t , const std::string& p) {
        this->onMessageReceived(t,p);
    });

    mqtt_client_->startLoop();

    mqtt_client_->subscribeTopic("veda/wearable/data");
    std::cout << "[MqttMasterManager] Subscribed to 'veda/wearable/data'" << std::endl;

    return true;
}


bool MqttMasterManager::checkFallStatus(const WearableData& data, AlarmCommand& out_cmd) {
    if(data.is_fall_detected) {
        out_cmd.target_device = "alarm_rpi_01";
        out_cmd.timestamp = data.timestamp;
        out_cmd.volume = 90;
        out_cmd.loop = true;

        out_cmd.status = "낙상"; // 프로토콜 정해지면 넣을께요 
        out_cmd.message = "낙상 감지";
        out_cmd.audio_action = "PLAY";
        out_cmd.audio_file = "fever_alert.wav";
        return true;
    }
    return false;
}


void MqttMasterManager::sendAlarmCommand(const AlarmCommand& cmd) {
    nlohmann::json j = cmd;
    std::string payload = j.dump();

    mqtt_client_->publishMessage("veda/alarm/control",payload);
    std::cout << "[MqttMasterManager] SentAlarm Command to Node: " << payload<< std::endl;
}

void MqttMasterManager::onMessageReceived(const std::string& topic, const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        auto data = j.get<WearableData>();

        std::cout << "[MqttMasterManager] Fall: " << (data.is_fall_detected ? "yes" : "no") << std::endl;
        
        AlarmCommand cmd;

        if (checkFallStatus(data,cmd)) {
            sendAlarmCommand(cmd);
        }
    } catch (const std::exception& e) {
        std::cerr << "[MqttMasterManager] Parsing Error: " << e.what() << std::endl;
    }
}


