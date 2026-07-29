#include "mqtt_qt_node.hpp"
#include <nlohmann/json.hpp> 
#include <QDebug> 

MqttQtManager::MqttQtManager(QObject* parent) :QObject(parent), m_client(nullptr), m_isConnected(false) {

}



bool MqttQtManager::init(const QString& broker_ip, int port, const QString& client_id ){
    if(m_isConnected) {
        qWarning() << "[MQTT] Already connected. Disconnecting first...";
        disconnectFromBroker();
    }

    try{
        std::string ip_std = broker_ip.toStdString();
        std::string id_std = client_id.toStdString();

        m_client = std::make_unique<MqttClient_veda>();

        bool result = m_client->connectToBroker(ip_std,port);

        if(result){
            m_isConnected = true;
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
        emit connectionError(QString::fromLocal8bit(e.what()));
        return false;
    } 

}


void MqttQtManager::disconnectFromBroker() {
    if(m_client && m_isConnected) {
        m_client->disconnect();
        m_isConnected = false;
        m_client.reset();
        qInfo() <<"[MQTT] Disconnected from broker";
        emit disconnected();
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



        

