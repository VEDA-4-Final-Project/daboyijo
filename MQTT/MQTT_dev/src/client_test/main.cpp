// test_tls_main.cpp
#include "MqttClient_veda.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    MqttClient_veda client("TLS_Test_Client");

    // 1. TLS 인증서 경로 등록 (경로는 실제 본인 ca.crt 위치로 지정)
    std::string ca_path = "/home/mayoina/study_veda/daboyijo/MQTT/certs/ca.crt";
    client.setTlsConfig(ca_path);

    // 2. 메시지 수신 콜백 등록
    client.setCallback([](const std::string& topic, const std::string& payload) {
        std::cout << "[ 수신 완료 ] Topic: " << topic << " | Payload: " << payload << std::endl;
    });

    // 3. MQTTS 포트(8883)로 로컬 접속 시도
    std::cout << "[Test] Connecting to MQTTS Broker..." << std::endl;
    if (!client.connectToBroker("127.0.0.1", 8883)) {
        std::cerr << "[Test] MQTTS Connection Failed!" << std::endl;
        return 1;
    }

    // 4. 이벤트 루프 시작
    client.startLoop();

    // 연결 안정화를 위해 잠시 대기 후 메시지 테스트 발행
    std::this_thread::sleep_for(std::chrono::seconds(1));
    client.publishMessage("veda/test/topic", "Hello Encrypted World!");

    // 5초간 대기하며 메시지 수신 확인
    std::this_thread::sleep_for(std::chrono::seconds(5));

    client.disconnect();
    return 0;
}
