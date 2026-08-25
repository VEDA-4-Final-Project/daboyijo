#ifndef AUDIOTRANSMITTER_H
#define AUDIOTRANSMITTER_H

#include <QObject>
#include <QAudioSource>
#include <QAudioFormat>
#include <QUdpSocket>
#include <QHostAddress>

class AudioTransmitter : public QObject {
    Q_OBJECT
public:
    explicit AudioTransmitter(QObject *parent = nullptr);
    ~AudioTransmitter();

    // 방송 시작/종료 함수
    bool startBroadcast(const QString &ip, quint16 port = 5000);
    void stopBroadcast();
    bool isBroadcasting() const;

signals:
    // UI에 상태 전달용 시그널
    void volumeChanged(int level);      // RMS 음량 (0 ~ 100)
    void broadcastStarted();
    void broadcastStopped();
    void errorOccurred(const QString &errorMessage);

private slots:
    void onReadyRead();

private:
    void initAudioFormat();
    int calculateRmsLevel(const QByteArray &data);

    QAudioFormat m_format;
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioDevice = nullptr;
    QUdpSocket *m_udpSocket = nullptr;

    QHostAddress m_targetIp;
    quint16 m_targetPort = 5000;
    bool m_isBroadcasting = false;
};

#endif // AUDIOTRANSMITTER_H
