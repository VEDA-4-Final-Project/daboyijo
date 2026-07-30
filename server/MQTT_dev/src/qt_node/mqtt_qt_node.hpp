#ifndef MQTT_QT_MODULE_H
#define MQTT_QT_MODULE_H

#include <QObject> 
#include <QString> 
#include <memory>
#include "MqttClient_veda.hpp" 

class MqttQtManager : QObject {
    Q_OBJECT
public:
    explicit MqttQtManager(QObject* parent = nullptr);
    ~MqttQtManager();
    
    //Mqtt 브로커 연결 
    bool init(const Qstring& broker_ip, int port = 1883, const QString& client_id= "");
    void disconnectFromBroker();

    // 명령 전송  
    void sendAlarmCommand(const AlarmCommand& cmd,int qos = 0);

signals:
    void connected();
    void disconnected();
    void connectionError(const Qstring& errorMsg);
    void alarmCommandReceived(const AlarmCommand& cmd);

private:
    std::unique_ptr<MqttClient_veda> m_client;
    bool m_isConnected{false};


};

#endif //MQTT_QT_MODULE_H

