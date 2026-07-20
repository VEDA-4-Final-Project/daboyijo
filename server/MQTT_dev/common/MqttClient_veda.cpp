#include "MqttClient_veda.hpp"
#include <iostream>
#include <mosquitto.h>


MqttClient_veda::MqttClient_veda(const std::string& id) : m_id(id), m_mosq(nullptr) {

    mosquitto_lib_init();
    m_mosq = mosquitto_new(m_id.c_str(), true, this);

    if(m_mosq) {
        mosquitto_connect_callback_set(m_mosq, MqttClient_veda::on_connect);
        mosquitto_message_callback_set(m_mosq, MqttClient_veda::on_message);
        mosquitto_disconnect_callback_set(m_mosq, MqttClient_veda::on_disconnect);

        mosquitto_reconnect_delay_set(m_mosq, 1, 10, true);
    } else {
        std::cerr << " Failed to create mosquitto "<< std::endl;
    }
    std::cout << "MQTT Client Initialized with ID: " << id << std::endl;

}


MqttClient_veda::~MqttClient_veda(){
    mosquitto_lib_cleanup();
    std::cout << "MQTT Client destroyed"<< std::endl;
}


void MqttClient_veda::initTest() {
    std::cout << "Mospuitto C API Link Test Success!" << std::endl;
}

bool MqttClient_veda::connectToBroker(const std::string& host, int port) {
    if(!m_mosq){
        return false;
    }

    int rc = mosquitto_connect(m_mosq, host.c_str(), port, 60);
    return (rc == MOSQ_ERR_SUCCESS);
}

void MqttClient_veda::disconnect() {
    if(m_mosq) {
        mosquitto_disconnect(m_mosq);
    }
}

void MqttClient_veda::startLoop() {
    if(m_mosq) {
        // 수신대기용 무한 루프 스레드를 백그라운드에 실행 
        int rc = mosquitto_loop_start(m_mosq);
        if(rc == MOSQ_ERR_SUCCESS) {
            std::cout << "[debug] mosquitto_loop_start 성공"<< std::endl;
        }else {
            std::cout << "[debug] mosquitto_loop_start 실패"<< std::endl;
        }

    }
}

void MqttClient_veda::stopLoop() {
    if(m_mosq){
        mosquitto_loop_stop(m_mosq, true);
    }
}

bool MqttClient_veda::publishMessage(const std::string& topic, const std::string& payload, int qos) {
    if(!m_mosq) {
        return false;
    }
    if(m_isConnected) {
        int rc = mosquitto_publish(m_mosq, nullptr , topic.c_str(), payload.length(), payload.c_str(), qos, false);
        
        if(rc == MOSQ_ERR_SUCCESS) {
            return true;
        }else {
            std::cout << "[MQTT] 즉시 전송 실패 (에러코드: " << rc << ")" << std::endl;
            return false;
        }
    }else {
        // 네트워크 오류로 끊어진 상태면 오프라인 큐에 저장 
        MqttMessage msg{topic,payload};

        m_offlineQueue.push(msg);

        std::cout << "[MQTT] [offline mode] 데이터를 큐에 저장했습니다. 현재 대기량: " << m_offlineQueue.size() <<"개 " << std::endl;

        return true;
    }
}

bool MqttClient_veda::subscribeTopic(const std::string& topic, int qos) {
    if(!m_mosq) {
        return false;
    }

    int rc = mosquitto_subscribe(m_mosq, nullptr, topic.c_str(), qos);
    return (rc == MOSQ_ERR_SUCCESS);
}

void MqttClient_veda::setCallback(MessageCallback callback) {
    m_userCallback = callback;
}


// === 내부 static 콜백 구현 ===

void MqttClient_veda::on_connect(struct mosquitto *mosq, void *obj, int rc) {
    if(rc == 0) {
        std::cout << "===> [MQTT] Successfully connected to broker" << std::endl;
        MqttClient_veda* client = static_cast<MqttClient_veda*>(obj);

        if(client) {
            client->subscribeTopic("veda/test/topic");
            //std::cout << "[DEBUG] on_connect 콜백 안에서 veda/test/topic 구독 완료!" << std::endl;
            client->m_isConnected = true;

            MqttMessage bufferedMessage;
            while (client->m_offlineQueue.tryPop(bufferedMessage)) {

                    client->publishMessage(bufferedMessage.topic, bufferedMessage.payload);
            }
        }

        
    } else {
        std::cerr << "===> [MQTT] Connection failed with code : "<< rc << std::endl;
    }
}

void MqttClient_veda::on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {

    // void* obj포인터를 MqttClient_veda 객체 타입으로 캐스팅 
    std::cout <<"[debug] on_message 트리거"<< std::endl;
    MqttClient_veda* client = static_cast<MqttClient_veda*>(obj);

    if(client && client->m_userCallback && message->payload) {
        std::cout <<"[debug] 객체 캐스팅성공 "<< std::endl;
        std::string topic(message->topic);
        std::string payload(static_cast<char*>(message->payload), message->payloadlen);

        client->m_userCallback(topic,payload);
    }
}

void MqttClient_veda::on_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    MqttClient_veda* client = static_cast<MqttClient_veda*>(obj);

    if(client) {
        client->m_isConnected = false;

        if(rc != 0) {
            std::cout << "===> [MQTT] Unexpected disconnect code : " << rc << ")" << std::endl;
        } else {
            std::cout << "===> [MQTT] Clean disconnect by user " << std::endl;
        }
    }
}





