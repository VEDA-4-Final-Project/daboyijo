#include "audiotransmitter.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDebug>
#include <cmath>

AudioTransmitter::AudioTransmitter(QObject *parent)
    : QObject(parent)
{
    initAudioFormat();
    m_udpSocket = new QUdpSocket(this);
}

AudioTransmitter::~AudioTransmitter()
{
    stopBroadcast();
}

void AudioTransmitter::initAudioFormat()
{
    // 검증 완료된 48kHz, Mono, Signed 16-bit PCM
    m_format.setSampleRate(48000);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);
}

bool AudioTransmitter::startBroadcast(const QString &ip, quint16 port)
{
    if (m_isBroadcasting) return true;

    m_targetIp = QHostAddress(ip);
    m_targetPort = port;

    QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    if (inputDevice.isNull()) {
        emit errorOccurred("사용 가능한 마이크 입력 장치를 찾을 수 없습니다.");
        return false;
    }

    m_audioSource = new QAudioSource(inputDevice, m_format, this);
    m_audioDevice = m_audioSource->start();

    if (!m_audioDevice) {
        emit errorOccurred("마이크 스트림을 시작하지 못했습니다.");
        delete m_audioSource;
        m_audioSource = nullptr;
        return false;
    }

    connect(m_audioDevice, &QIODevice::readyRead, this, &AudioTransmitter::onReadyRead);

    m_isBroadcasting = true;
    emit broadcastStarted();
    return true;
}

void AudioTransmitter::stopBroadcast()
{
    if (!m_isBroadcasting) return;

    m_isBroadcasting = false;

    if (m_audioSource) {
        m_audioSource->stop();
        m_audioSource->deleteLater();
        m_audioSource = nullptr;
        m_audioDevice = nullptr;
    }

    emit volumeChanged(0);
    emit broadcastStopped();
}

bool AudioTransmitter::isBroadcasting() const
{
    return m_isBroadcasting;
}

void AudioTransmitter::onReadyRead()
{
    if (!m_audioDevice || !m_isBroadcasting) return;

    QByteArray buffer = m_audioDevice->readAll();
    if (buffer.isEmpty()) return;

    // 1. RMS 음량 계산 후 UI로 시그널 방출
    int level = calculateRmsLevel(buffer);
    emit volumeChanged(level);

    // 2. UDP 송신
    m_udpSocket->writeDatagram(buffer, m_targetIp, m_targetPort);
}

int AudioTransmitter::calculateRmsLevel(const QByteArray &data)
{
    const qint16 *samples = reinterpret_cast<const qint16*>(data.constData());
    int sampleCount = data.size() / sizeof(qint16);
    if (sampleCount == 0) return 0;

    double sum = 0;
    for (int i = 0; i < sampleCount; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }

    double rms = std::sqrt(sum / sampleCount);
    return static_cast<int>((rms / 32767.0) * 100);
}