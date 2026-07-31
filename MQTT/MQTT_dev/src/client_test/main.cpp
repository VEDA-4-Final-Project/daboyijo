#include "MqttClient_veda.hpp"
#include <iostream>
#include <unistd.h>


void onMessageReceived(const std::string& topic, const std::string&payload) {
    std::cout << "[Received] topic: "<< topic << " | message: " << payload << std::endl;
}


int main() {
    MqttClient_veda client("Test_Node");

    client.setCallback(onMessageReceived);

    std::cout << "Connecting to broker..." << std::endl; 
    if(!client.connectToBroker("localhost")) {
        std::cerr << "Failed to connect to broker. is mosquitoo service runninng? " << std::endl;
        return 1;
    }

    client.startLoop();
    usleep(500000);
    client.subscribeTopic("veda/test/topic");

    std::cout << "Loop started. exit is ctrl + c " << std::endl;

    int count = 0;
    while (true) {
        WearableData mock_sensor_data;
        mock_sensor_data.device_id = "wearable_rpi_01";
        mock_sensor_data.temperature = 36.6 + (count%3) *0.2;
        mock_sensor_data.heart_rate = 75 + (count % 5);
        mock_sensor_data.is_fall_detected = ((count % 10)==0);
        mock_sensor_data.timestamp = 1830000+ count;

        nlohmann::json j = mock_sensor_data;
        std::string serialized_payload = j.dump();

        client.publishMessage("veda/test/topic",serialized_payload);
        std::cout << "[publish] sent json paload (Count : " << count++ <<")" << std::endl;

        sleep(2);
    }

    client.stopLoop();

    return 0;
}


