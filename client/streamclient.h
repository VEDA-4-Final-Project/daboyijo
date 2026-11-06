#ifndef STREAMCLIENT_H
#define STREAMCLIENT_H

// ═══════════════════════════════════════════════════════════
//  StreamClient — 영상 서버(Pi) 통신 전담
// ═══════════════════════════════════════════════════════════
// 예전엔 MainWindow 안에 소켓·수신 버퍼·패킷 파싱·제어 메시지 조립이 화면 코드와
// 섞여 있었다. 화면(위젯이든 QML이든)은 "무엇을 보여줄지"만 알고, 바이트를 어떻게
// 주고받는지는 몰라도 되도록 통신만 이 클래스로 떼어 냈다.
//
//   · 받기: 프레임/이벤트/검색결과를 파싱해 시그널로 내보낸다(UI 위젯을 모름).
//   · 보내기: send*() 가 제어 패킷을 조립해 담당 Pi 소켓으로 보낸다.
//             연결이 안 돼 있으면 false — 경고 문구는 호출한 화면이 정한다.
//   · 연결: 2-Pi 분할(ch0·1 → Pi A, ch2·3 → Pi B), TLS(자체 CA), 끊기면 자동 재접속.
//
// QObject + 시그널 구조라 나중에 QML로 화면을 바꿔도 이 클래스는 그대로 쓴다
// (QML 쪽은 이 시그널에 연결하거나 Q_PROPERTY로 감싸기만 하면 된다).

#include <QObject>
#include <QAbstractSocket>
#include <QByteArray>
#include <QImage>
#include <QPolygonF>
#include <QString>
#include <QTimer>

class QSslSocket;

class StreamClient : public QObject
{
    Q_OBJECT

public:
    static constexpr int kNumServers = 2;    // Pi A, Pi B
    static constexpr int kNumChannels = 4;
    static constexpr quint16 kServerPort = 5500;

    // 채널 → 담당 Pi 인덱스 (각 Pi의 cameras.conf 채널 선언과 일치해야 한다)
    static int serverForChannel(int ch) { return ch < 2 ? 0 : 1; }
    // Pi 인덱스 → 접속 주소 (QSettings 저장값, 없으면 기본값)
    static QString serverHost(int idx);

    explicit StreamClient(QObject* parent = nullptr);
    ~StreamClient() override;

    void start();   // 소켓 생성 + 첫 접속 (이후 끊기면 자동 재접속)
    void stop();    // 신호 차단 후 즉시 끊기 (창 종료 직전에 호출)

    QAbstractSocket::SocketState serverState(int idx) const;
    bool isServerConnected(int idx) const;
    bool isChannelConnected(int ch) const { return isServerConnected(serverForChannel(ch)); }
    int  connectedServerCount() const;

    // 이 채널 프레임을 지금 화면에 그리지 않을 때 — 디코드 자체를 건너뛴다.
    void setChannelSuppressed(int ch, bool suppressed);

    // ── 제어 메시지 (Qt → 서버). 담당 Pi에 연결돼 있지 않으면 false ──
    bool sendRoi(int ch, int roiId, const QPolygonF& normPts, bool clear);
    bool sendRoiBind(int ch, int roiId, int residentId);
    bool sendCameraSet(int ch, const QString& rtspUrl);
    bool sendCameraClear(int ch);
    bool sendImageParams(int ch, int brightness, int contrast, int saturation);
    bool sendFocus(int ch, bool area, float nx, float ny);
    bool sendAlarmConfirm(int ch);                       // 경보 확인 → 모자이크 복구
    bool sendRiskLevel(int ch, int roiId, int level);    // level: 1하·2중·3상
    // ch < 0 이면 전체 채널 검색 — 연결된 아무 Pi(A 우선)로 보낸다(DB를 공유하므로).
    bool sendSearchQuery(int ch, const QString& query);

signals:
    // 채널별 "가장 최신" 프레임만 (backlog 프레임은 디코드 없이 버린다)
    void frameReceived(int channel, const QImage& image, qint64 serverTimestampMs);
    // roiId: 서버가 보낸 침대 번호 그대로(0xFF = 미상)
    void fallEvent(int channel, int roiId, quint64 timestampMs, float nx, float ny);
    void bedEgressEvent(int channel, int roiId, quint64 timestampMs, float nx, float ny);
    void vitalAbnormalEvent(int channel, quint64 timestampMs);
    void searchResultReceived(int channel, const QString& text);
    // 어떤 소켓이든 상태가 바뀌면 — 집계는 connectedServerCount()/isChannelConnected()로
    void connectionStateChanged();
    // Pi가 새로 연결됨(끊김 → 연결) — 서버는 재접속 후 카메라를 모르므로 재전송용
    void serverConnected(int serverIdx);

private slots:
    void onReadyRead();
    void onSocketStateChanged();
    void connectToServers();   // 끊긴 소켓만 골라 (재)접속

private:
    QSslSocket* socketForServer(int idx) const;
    bool writeToServer(int idx, const QByteArray& pkt);
    bool writeToChannel(int ch, const QByteArray& pkt);
    static QByteArray ctrlHeader(uint8_t type, int ch, uint8_t pointCount, uint16_t reserved);

    QSslSocket* sockets_[kNumServers] = {};
    QByteArray  buffers_[kNumServers];          // 연결마다 바이트 스트림이 별개 → 버퍼도 분리
    bool        wasConnected_[kNumServers] = {};
    bool        suppressed_[kNumChannels] = {};
    QTimer      reconnectTimer_;
};

#endif // STREAMCLIENT_H
