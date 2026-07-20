#include "MqttServerModule.hpp"
#include <iostream>
#include <nlohmann/json.hpp>

MqttServerModule::MqttServerModule() {
        mqtt_client_ = std::make_unique<MqttClient_veda>("Main_Server_Module");
}

MqttServerModule::~MqttServerModule() {
    if(mqtt_client_) {
        mqtt_client_->stopLoop();
    }
}

bool MqttServerModule::init(const std::string& broker_ip, int port) {
    if(!mqtt_client_->connectToBroker(broker_ip, port)) {
        std::cerr << "[MqttServerModule] Broker connection failed!" << std::endl;
        return false;
    }

    mqtt_client_->setMessageCallback([this](const::std::string& t , const std::string& p) {
        this->onMessageReceived(t,p);
    });

    mqtt_client_->startLoop();

    mqtt_client_->subscribeTopic("veda/wearable/data");
    std::cout << "[MqttServerModule] Subscribed to 'veda/wearable/data'" << std::endl;

    return true;
}


bool MqttServerModule::checkFallStatus(const WearableData& data, AlarmComand& out_cmd) {
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


void MqttServerModule::sendAlarmCommand(const AlarmComand& cmd) {
    nlohmann::json j = cmd;
    std::string payload = j.dump();

    mqtt_client_->publishMessage("veda/alarm/control",payload);
    std::cout << "[MqttServerModule] SentAlarm Command to Node: " << payload<< std::endl;
}

void MqttServerModule::onMessageReceived(const std::string& topic, const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        auto data = j.get<WearableData>();

        std::cout << "[MqttServerModule] Fall: " << (data.is_fall_detected ? "yes" : "no") << std::endl;
        
        AlarmComand cmd;

        if (checkFallStatus(data,cmd)) {
            sendAlarmCommand(cmd);
        }
    } catch (const std::exception& e) {
        std::cerr << "[MqttServerModule] Parsing Error: " << e.what() << std::endl;
    }
}


   


