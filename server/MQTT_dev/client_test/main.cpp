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
        std::string message = "Hello veda daboyjo! Count: " + std::to_string(count++);

        client.publishMessage("veda/test/topic",message);
        std::cout << "[publish] " << message << std::endl;

        sleep(2);
    }

    client.stopLoop();

    return 0;
}


