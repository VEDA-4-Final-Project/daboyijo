#include "MqttClient_veda.cpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include "VedaAudioPlayer.hpp"

const std::string TEST_DEVICE_ID = "alarm_rpi_01";

void onAlarmCommandReceived(const std::string& topic, const std::string& payload) {
    try{
        auto j = nlohmann::json::parse(payload);
        auto cmd = j.get<AlarmCommand>();

        if(cmd.target_device == TEST_DEVICE_ID){
            if(cmd.audio_action == "PLAY") {
                std::cout << "[Alarm node] Play sound: "<< cmd.audio_file << std::endl;
                VedaAudioPlayer player();
                int ret = player.playWav(cmd.audio_file);
                if(ret <0) {
                    std::cerr<< "[Alarm node] sound error" << std::endl;
                }
            }
                    

        }


    }catch (std::exception & e){
        std::cerr << "[Alarm node] json parsing error: " << e.what() << std::endl;
    }
}

int main() {
    MqttClient_veda client("Alarm_Node");

    std::cout << "[Alarm node] Connecting to mqtt broker... " << std::endl;
    if(!client.connectToBroker("localhost",1883)) {
        std::cerr << "[Alarm node] Connection failed" << std::endl;
        return 1;
    }


    client.setCallback(onAlarmCommandReceived);
    client.startLoop();


    client.subscribeTopic("veda/alarm/control",0);
    std::cout << "[Alarm node] Active. Subscribbed to 'veda/alarm/control'" << std::endl;

    while(true) {
        sleep(1);
    }
    client.stopLoop();
    return 0;
}

        
            
        
