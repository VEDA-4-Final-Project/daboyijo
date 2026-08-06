#include <iostream>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "MqttClient_veda.hpp"

int main() {
    MqttClient_veda client("Wearable_Gateway_Node");

    std::cout << "[Wearable Node] Connecting to Mqtt broker..." << std::endl;

    if(!client.connectToBroker("172.20.32.10",1883)) {
        std::cerr <<"[Wearable Node] Mqtt connection failed!" << std::endl;
        return 1;
    }

    client.startLoop();
    std::cout << "[Wearable Node] Setup complete. engine runningg: " << std::endl;

    int count = 0;

    while(true) {
        // 추후 stm32 블루투스 프로토콜 확정시 수신 및 파싱 로직 적용 예정 
        WearableData sensor_data;
        sensor_data.device_id = "wearable_rpi_01";
        sensor_data.temperature = 36.5 + (count % 3) *0.1;
        sensor_data.heart_rate = 75 + (count % 8);
        sensor_data.is_fall_detected = (count % 10 == 0 && count != 0);
        sensor_data.timestamp = 1830000 + count++;

        nlohmann::json j = sensor_data;
        std::string payload = j.dump();

        client.publishMessage("veda/wearable/data",payload);

        std:: cout << "[Wearable Node] Published JSON: " <<payload <<  std::endl;
        sleep(1);
    }

    client.stopLoop();
    return 0;
}

