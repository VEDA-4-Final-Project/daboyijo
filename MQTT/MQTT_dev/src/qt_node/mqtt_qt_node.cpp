#include "mqtt_qt_node.hpp"
#include <nlohmann/json.hpp> 
#include <QDebug> 

MqttQtManager::MqttQtManager(QObject* parent) :QObject(parent), m_client(nullptr), m_isConnected(false) {

}

MqttQtManager::~MqttQtManager() {
    disconnectFromBroker();
}


bool MqttQtManager::init(const QString& broker_ip, int port, const QString& client_id ){
    if(m_isConnected) {
        qWarning() << "[MQTT] Already connected. Disconnecting first...";
        disconnectFromBroker();
    }

    try{
        std::string ip_std = broker_ip.toStdString();
        std::string id_std = client_id.toStdString();

        m_client = std::make_unique<MqttClient_veda>(id_std.empty() ? "Qt_Control_Node" : id_std);

        m_client->setCallback([this](const std::string& topic, const std::string& payload) {
            this->handleIncomingMessage(topic,payload);
        });

        bool result = m_client->connectToBroker(ip_std,port);

        if(result){
            m_isConnected = true;
               
            m_client->subscribeTopic("veda/wearable/data");
            m_client->startLoop();


            qInfo() <<"[MQTT] Connected to " << broker_ip << ": " << port ;
            emit connected();
            return true;
        } else {
            qWarning() <<"[MQTT] Connection fail to " << broker_ip;
            emit connectionError("Failed to connct tobroker" );
            return false;
        }
    }catch(const std::exception& e) {
        qCritical() << "[MQTT] Exception during init : " << e.what();
        emit connectionError(QString::fromLocal8Bit(e.what()));
        return false;
    } 

}


void MqttQtManager::disconnectFromBroker() {
    if(m_client && m_isConnected) {
        m_client->stopLoop();
        m_client->disconnect();
        m_isConnected = false;
        m_client.reset();
        qInfo() <<"[MQTT] Disconnected from broker";
        emit disconnected();
    }
}


void MqttQtManager::handleIncomingMessage(const std::string& topic, const std::string& payload){
    try {
        auto j = nlohmann::json::parse(payload);

        if (topic == "veda/wearable/data") {
            auto data = j.get<WearableData>();

            emit wearableDataReceived(data);
        }
    } catch (const std::exception& e) {
        qWarning() << "[MQTT] JSON Parse Error on topic" << QString::fromStdString(topic) << ":" << e.what();
    }
}


void MqttQtManager::sendAlarmCommand(const AlarmCommand& cmd,int qos) {
    if(!m_isConnected || !m_client) {
        qWarning() << "[MQTT] Cannot send command : Not connected to broker";
        return ;
    }
    try {
        //  알람 커멘드 구조체를 json 객체로 직렬화 
        nlohmann::json j = cmd;


        // 직렬화 한것을 std::string객체로 변환 
        std::string payload = j.dump();

        //토픽 설정 (여러군대 보내야 한다면 이걸 인자로 바꿔야 할것 같습니다. 
        std::string topic = "veda/alarm/control";


        bool result = m_client->publishMessage(topic, payload, qos);

        if(result) {
            qInfo() << "[MQTT] Alarm command published successfully. Topic : " << QString::fromStdString(topic) << " QoS: " << qos;
        } else {
            qWarning()<< "[MQTT] Failed to publish alarm command." ;
        }
    }catch (const std::exception& e) {
        qCritical() << "[MQTT] Exception during sendAlarmCommand : " << e.what();
    } 
}

