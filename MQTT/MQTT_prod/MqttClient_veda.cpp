#include "MqttClient_veda.hpp"
#include <iostream>
#include <mosquitto.h>


MqttClient_veda::MqttClient_veda(const std::string& id) : m_isConnected(false), m_mosq(nullptr), m_id(id) {

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
    stopLoop();
    disconnect();
    if(m_mosq) {
       mosquitto_destroy(m_mosq);
       m_mosq = nullptr;
    }
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
    
    // TLS 설정이 활성화도니 경우 연결 직전에 적용 
    if(m_use_tls) {
        if(m_ca_path.empty()) {
            std::cerr << "[MQTT Client] Error: TLS is enabled but CA path is empty!" << std::endl;
            return false;
        }


        // CA 루트 인증서 경로 설정 
        int rc = mosquitto_tls_set(m_mosq, m_ca_path.c_str(), nullptr, nullptr, nullptr, nullptr);
        if(rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "[MQTT Client] TLS Set Error: " << mosquitto_strerror(rc) << std::endl;
            return false;
        }
       
        // IP 변경 대응을 위해 호스트명 검증 비활성화 
        mosquitto_tls_insecure_set(m_mosq, true);
        std::cout << "[MQTT Client] TLS Configured (CA: " << m_ca_path << ")" <<std::endl;
    }
        
    int rc = mosquitto_connect(m_mosq, host.c_str(), port, 60);
    if(rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT Client] 최초 연결 실패: " << mosquitto_strerror(rc)
                  << " — 비동기 재접속으로 전환합니다." << std::endl;
        // 루프 스레드가 접속을 계속 재시도하도록 예약해둔다.
        // (mosquitto_loop_start 의 loop_forever 가 connect_async 상태를 보고
        //  reconnect_delay 백오프로 재시도한다. 이걸 안 걸어두면 최초 연결이
        //  실패한 뒤로는 아무도 다시 시도하지 않는다.)
        mosquitto_connect_async(m_mosq, host.c_str(), port, 60);
        return false;
    }
    return true;
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

PublishResult MqttClient_veda::publish(const std::string& topic, const std::string& payload, int qos) {
    if(!m_mosq) {
        return PublishResult::Failed;
    }
    if(m_isConnected) {
        int rc = mosquitto_publish(m_mosq, nullptr , topic.c_str(), payload.length(), payload.c_str(), qos, false);

        if(rc == MOSQ_ERR_SUCCESS) {
            return PublishResult::Sent;
        }else {
            std::cout << "[MQTT] 즉시 전송 실패 (에러코드: " << rc << ")" << std::endl;
            return PublishResult::Failed;
        }
    }else {
        // 네트워크 오류로 끊어진 상태면 오프라인 큐에 저장
        MqttMessage msg{topic,payload};

        m_offlineQueue.push(msg);

        std::cout << "[MQTT] [offline mode] 아직 전송되지 않았습니다 — 큐에 보관. 현재 대기량: "
                  << m_offlineQueue.size() <<"개 (topic: " << topic << ")" << std::endl;

        return PublishResult::Queued;
    }
}

// 하위호환 래퍼 — 큐에 담기만 한 경우는 false 다. 예전엔 여기서 true 를 돌려줘
// 호출부가 "보냈다"고 로그를 찍는 원인이 됐다.
bool MqttClient_veda::publishMessage(const std::string& topic, const std::string& payload, int qos) {
    return publish(topic, payload, qos) == PublishResult::Sent;
}

// 실제 SUBSCRIBE 패킷만 보낸다(목록 기록은 호출부인 subscribeTopic/on_connect 담당).
bool MqttClient_veda::sendSubscribe(const std::string& topic, int qos) {
    if(!m_mosq) {
        return false;
    }
    int rc = mosquitto_subscribe(m_mosq, nullptr, topic.c_str(), qos);
    if(rc != MOSQ_ERR_SUCCESS) {
        std::cout << "[MQTT] 구독 실패 (" << topic << ", 에러코드: " << rc << ")" << std::endl;
        return false;
    }
    return true;
}

bool MqttClient_veda::subscribeTopic(const std::string& topic, int qos) {
    if(!m_mosq) {
        return false;
    }

    // 재접속 때 다시 걸 수 있도록 먼저 기록한다. 아직 연결 전이어도 마찬가지 —
    // on_connect 가 이 목록을 보고 걸어준다.
    {
        std::lock_guard<std::mutex> lock(m_subMutex);
        bool known = false;
        for(auto& s : m_subscriptions) {
            if(s.first == topic) { s.second = qos; known = true; break; }
        }
        if(!known) {
            m_subscriptions.emplace_back(topic, qos);
        }
    }

    if(!m_isConnected) {
        std::cout << "[MQTT] 아직 연결 전 — 구독 예약: " << topic << std::endl;
        return true;   // 연결되면 on_connect 가 걸어준다
    }
    return sendSubscribe(topic, qos);
}

void MqttClient_veda::setCallback(MessageCallback callback) {
    m_userCallback = callback;
}


// === 내부 static 콜백 구현 ===

// mosq 는 mosquitto 콜백 시그니처가 요구할 뿐 안 씀 — 객체는 obj 로 되찾음
void MqttClient_veda::on_connect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
    if(rc == 0) {
        std::cout << "===> [MQTT] Successfully connected to broker" << std::endl;
        MqttClient_veda* client = static_cast<MqttClient_veda*>(obj);

        if(client) {
            client->m_isConnected = true;

            // 끊겼다 붙은 것일 수도 있다 — clean session 이라 브로커 쪽 구독이
            // 날아갔으므로 등록해둔 토픽을 전부 다시 건다. (안 그러면 재접속 후
            // 조용히 아무것도 안 들어온다)
            std::vector<std::pair<std::string, int>> subs;
            {
                std::lock_guard<std::mutex> lock(client->m_subMutex);
                subs = client->m_subscriptions;
            }
            for(const auto& s : subs) {
                if(client->sendSubscribe(s.first, s.second)) {
                    std::cout << "[MQTT] 구독 복구: " << s.first << std::endl;
                }
            }

            // 오프라인 동안 쌓인 것들을 흘려보낸다.
            int flushed = 0;
            MqttMessage bufferedMessage;
            while (client->m_offlineQueue.tryPop(bufferedMessage)) {
                if(client->publish(bufferedMessage.topic, bufferedMessage.payload)
                       == PublishResult::Sent) {
                    ++flushed;
                }
            }
            if(flushed > 0) {
                // ⚠️ 큐에 오래 묵은 알람도 지금 그대로 나간다 — 20분 전 낙상이
                //    이 순간 사이렌을 울린다. TTL 도입은 별도 과제.
                std::cout << "[MQTT] 오프라인 큐 " << flushed << "개 재전송 완료" << std::endl;
            }
        }

        
    } else {
        std::cerr << "===> [MQTT] Connection failed with code : "<< rc << std::endl;
    }
}

void MqttClient_veda::on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
    (void)mosq;

    // void* obj포인터를 MqttClient_veda 객체 타입으로 캐스팅
    //std::cout <<"[debug] on_message 트리거"<< std::endl;
    MqttClient_veda* client = static_cast<MqttClient_veda*>(obj);

    if(client && client->m_userCallback && message->payload) {
        //std::cout <<"[debug] 객체 캐스팅성공 "<< std::endl;
        std::string topic(message->topic);
        std::string payload(static_cast<char*>(message->payload), message->payloadlen);

        client->m_userCallback(topic,payload);
    }
}

void MqttClient_veda::on_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
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



void MqttClient_veda::setTlsConfig(const std::string& ca_path) {
    m_use_tls = true;
    m_ca_path = ca_path;
}





