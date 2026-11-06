#include "streamclient.h"
#include "streamprotocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QHostAddress>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QtGlobal>

#include <cstring>

namespace {

// 영상 서버 접속 정보 (RPi 주소) — 2-Pi 분할: 채널을 두 라즈베리에 2+2로 나눠 서빙.
//   Pi A = ch0·ch1, Pi B = ch2·ch3. 각 Pi의 cameras.conf 채널 번호가 아래 인덱스와
//   일치해야 하고(serverForChannel), Qt는 두 IP에 각각 붙는다.
// 하드코딩 대신 QSettings(관제 PC 로컬)에 저장 — 기본값은 아래 상수이며, 관제
// PC마다 다른 Pi를 볼 수 있게 설정에서 바꿀 수 있다. (CCTV IP와는 별개: 이건 Qt가
// "붙는 서버" 주소이고, CCTV는 서버가 여는 카메라 주소다.)
const char* kSettingsHostA = "server/hostA";     // Pi A (ch0·ch1)
const char* kSettingsHostB = "server/hostB";     // Pi B (ch2·ch3)
const char* kDefaultHostA  = "172.20.32.79";
const char* kDefaultHostB  = "172.20.32.80";

constexpr int kReconnectDelayMs = 3000;   // 끊김 후 재접속 간격

// JPEG 페이로드 크기 상한 — 960x540 q80 실측 수십 KB 수준이라 4MB면 충분.
// 이걸 넘는 payload_len은 스트림 오염(또는 프로토콜 불일치)으로 본다.
constexpr quint32 kMaxPayloadLen = 4 * 1024 * 1024;

// 영상 스트림 서버(Pi) 검증용 CA. MQTT 브로커와 같은 CA(DavoCA)로 서명하므로
// 새 인증서를 발급해도 이 파일 하나만 certs/에 있으면 되고, 배포된 클라이언트를
// 다시 건드릴 일이 없다.
// MQTT의 certs/ca.crt와 일부러 다른 파일명을 쓴다. 같은 파일을 공유하면(예전
// 방식) 영상 스트림 TLS를 끄려고 이 파일을 지웠을 때 MqttQtManager까지 같이
// 끊긴다 — 그쪽은 경로 문자열이 비어있을 때만 평문으로 내려가고, 파일이 없으면
// (경로는 있는데 못 읽으면) 조용히 평문으로 안 가고 아예 연결 실패로 처리하기
// 때문이다. 그래서 두 인증서 파일을 분리해 "영상만 평문으로" 끌 수 있게 한다.
QString streamCaPath() {
    QSettings s;
    return s.value(QStringLiteral("stream/caCert"),
                   QCoreApplication::applicationDirPath()
                       + QStringLiteral("/certs/stream-ca.crt")).toString();
}

// 영상 서버(Pi) 인증서에 적힌 이름(CN). MQTT/scripts/generate_stream_certs.sh 의
//   openssl req -new -key streamA.key ... -subj "/CN=DaboStreamA"
// 와 반드시 같아야 한다(인덱스: 0=Pi A, 1=Pi B).
const char* kStreamCommonName[StreamClient::kNumServers] = {"DaboStreamA", "DaboStreamB"};

}  // namespace

QString StreamClient::serverHost(int idx)
{
    QSettings s;
    return idx == 0 ? s.value(kSettingsHostA, kDefaultHostA).toString()
                    : s.value(kSettingsHostB, kDefaultHostB).toString();
}

StreamClient::StreamClient(QObject* parent) : QObject(parent)
{
    reconnectTimer_.setSingleShot(true);
    reconnectTimer_.setInterval(kReconnectDelayMs);
    connect(&reconnectTimer_, &QTimer::timeout, this, &StreamClient::connectToServers);
}

StreamClient::~StreamClient()
{
    stop();
}

void StreamClient::start()
{
    // 서버별 소켓 생성 및 시그널 연결 (2-Pi: 소켓 kNumServers개)
    // 수신 슬롯 onReadyRead는 sender()로 어느 소켓이 신호를 냈는지 구분한다.
    for (int i = 0; i < kNumServers; ++i) {
        if (sockets_[i]) continue;
        sockets_[i] = new QSslSocket(this);
        QSslSocket* sock = sockets_[i];
        connect(sock, &QTcpSocket::readyRead, this, &StreamClient::onReadyRead);
        connect(sock, &QTcpSocket::stateChanged, this, &StreamClient::onSocketStateChanged);
        // MQTT(mqttqtmanager.cpp)와 같은 이유의 같은 예외 처리 — 우리 서버 인증서는
        // 공인 CA가 아니라 자체 CA(DavoCA)가 서명했고, IP로 접속하는데 인증서 SAN이
        // 그 IP와 완전히 일치하지 않을 수 있어 이름불일치(HostNameMismatch)만 딱
        // 짚어 CN이 기대값(DaboStreamA/B)일 때만 눈감아준다. 그 외 에러(서명 위조,
        // 만료 등)는 그대로 연결 실패로 남는다.
        connect(sock, &QSslSocket::sslErrors, this,
                [sock, i](const QList<QSslError>& errors) {
                    QList<QSslError> ignorable;
                    for (const QSslError& e : errors) {
                        if (e.error() != QSslError::HostNameMismatch) continue;
                        const QStringList cn =
                            e.certificate().subjectInfo(QSslCertificate::CommonName);
                        if (cn.contains(QLatin1String(kStreamCommonName[i]))) {
                            ignorable.append(e);
                        } else {
                            qWarning() << "[영상서버] 인증서 이름이 다릅니다. 기대:"
                                       << kStreamCommonName[i] << "실제:" << cn;
                        }
                    }
                    if (!ignorable.isEmpty()) sock->ignoreSslErrors(ignorable);
                });
    }
    // 명세서 스펙: 5500번 포트로 즉시 접속. 끊기면 reconnectTimer_가
    // kReconnectDelayMs 후 재접속 (24시간 무인 관제용)
    connectToServers();
}

void StreamClient::stop()
{
    reconnectTimer_.stop();
    // 소켓은 이 객체의 자식이라 파괴 단계에서 마지막 stateChanged(UnconnectedState)를
    // 한 번 더 낸다. 신호선을 먼저 끊고 나서 abort해야 반쯤 파괴된 객체의 슬롯을
    // 타지 않는다(예전 MainWindow 소멸자에서 qFatal로 죽던 원인과 같은 이야기).
    for (QSslSocket*& s : sockets_) {
        if (!s) continue;
        s->disconnect(this);
        s->abort();
    }
}

QSslSocket* StreamClient::socketForServer(int idx) const
{
    return (idx >= 0 && idx < kNumServers) ? sockets_[idx] : nullptr;
}

QAbstractSocket::SocketState StreamClient::serverState(int idx) const
{
    QSslSocket* s = socketForServer(idx);
    return s ? s->state() : QAbstractSocket::UnconnectedState;
}

bool StreamClient::isServerConnected(int idx) const
{
    return serverState(idx) == QAbstractSocket::ConnectedState;
}

int StreamClient::connectedServerCount() const
{
    int n = 0;
    for (int i = 0; i < kNumServers; ++i)
        if (isServerConnected(i)) ++n;
    return n;
}

void StreamClient::setChannelSuppressed(int ch, bool suppressed)
{
    if (ch >= 0 && ch < kNumChannels) suppressed_[ch] = suppressed;
}

// ═══════════════════════════════════════════════════════════
//  접속 / 상태
// ═══════════════════════════════════════════════════════════
void StreamClient::connectToServers()
{
    // 끊겨 있는 서버 소켓만 골라 재접속 (일부 Pi만 끊긴 경우도 처리)
    for (int i = 0; i < kNumServers; ++i) {
        if (!sockets_[i]) continue;
        if (sockets_[i]->state() != QAbstractSocket::UnconnectedState) continue;
        buffers_[i].clear();  // 이전 연결의 파싱 잔여물 폐기
        const QString host = serverHost(i);

        // TLS/평문은 CA 파일 유무로 가른다 — MqttQtManager::startConnection()의
        // m_caPath.isEmpty() 판단과 같은 패턴. certs/stream-ca.crt를 배포했다는 것
        // 자체가 "이 Pi들은 stream_cert_path를 켜서 TLS로 띄웠다"는 선언으로 본다.
        // 아직 인증서를 안 놓은 개발 환경/미전환 Pi에서는 파일이 없어 평문 접속.
        const QString caPath = streamCaPath();
        if (QFile::exists(caPath)) {
            QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
            const QList<QSslCertificate> ca = QSslCertificate::fromPath(caPath);
            if (ca.isEmpty()) {
                qWarning() << "[영상서버] CA 인증서를 읽지 못했습니다:" << caPath;
            } else {
                conf.setCaCertificates(ca);
            }
            conf.setPeerVerifyMode(QSslSocket::VerifyPeer);
            sockets_[i]->setSslConfiguration(conf);
            sockets_[i]->connectToHostEncrypted(host, kServerPort);
            qDebug() << "영상 서버 접속 시도(TLS):" << host << ":" << kServerPort;
        } else {
            sockets_[i]->connectToHost(QHostAddress(host), kServerPort);
            qDebug() << "영상 서버 접속 시도(평문):" << host << ":" << kServerPort;
        }
    }
}

void StreamClient::onSocketStateChanged()
{
    // 화면 갱신(연결 표시·LIVE 배지)은 받는 쪽이 집계 getter로 알아서 한다.
    emit connectionStateChanged();

    // Pi가 새로 연결됨(false→true)을 감지 → 저장된 카메라 재전송을 요청.
    // (서버는 재접속 후 카메라를 모르므로, 사용자가 다시 누르지 않아도 복구된다.)
    int unconnected = 0;
    for (int i = 0; i < kNumServers; ++i) {
        const auto st = serverState(i);
        if (st == QAbstractSocket::UnconnectedState) ++unconnected;
        const bool now = (st == QAbstractSocket::ConnectedState);
        if (now && !wasConnected_[i]) {
            wasConnected_[i] = true;
            emit serverConnected(i);
        }
        wasConnected_[i] = now;
    }

    // 끊긴 소켓이 하나라도 있으면 재접속 예약
    if (unconnected > 0 && !reconnectTimer_.isActive())
        reconnectTimer_.start();
}

// ═══════════════════════════════════════════════════════════
//  영상 수신 (명세서 프로토콜 파싱)
// ═══════════════════════════════════════════════════════════
void StreamClient::onReadyRead()
{
    // 어느 서버 소켓(Pi)이 신호를 냈는지 판별 → 그 소켓 전용 버퍼로 파싱한다.
    auto* sock = qobject_cast<QSslSocket*>(sender());
    if (!sock) return;
    int idx = 0;
    for (int i = 0; i < kNumServers; ++i)
        if (sock == sockets_[i]) { idx = i; break; }
    QByteArray& buffer = buffers_[idx];

    // 들어온 데이터를 무조건 (이 소켓의) 버퍼 뒤에 붙임
    buffer.append(sock->readAll());

    // 지연(백로그) 제거: 이번 호출에 도착한 프레임을 채널별로 훑어
    // "가장 최신 1장"만 남긴다. 그 사이 오래된 프레임은 디코드·렌더를
    // 아예 건너뛴다(화면엔 어차피 최신 것만 보이므로 헛일). 이렇게 하면
    // 소비자(GUI 스레드)가 유입 속도를 못 따라가도 backlog가 쌓이지 않는다.
    // ※ 이벤트(낙상 등)는 절대 스킵하지 않고 파싱 즉시 처리한다.
    QByteArray latestJpeg[kNumChannels];
    quint64    latestTs[kNumChannels] = {0, 0, 0, 0};
    bool       hasFrame[kNumChannels] = {false, false, false, false};

    // 버퍼에 완성된 패킷이 남아있는 동안 반복 파싱
    while (true) {
        // 0) 매직(2바이트)으로 패킷 종류 식별 — 영상(0xDB4B) / 이벤트(0xDB4D) / 검색(0xDB4E)
        if (buffer.size() < (int)sizeof(uint16_t))
            break;
        uint16_t magic;
        memcpy(&magic, buffer.constData(), sizeof(magic));

        // ── 이벤트 패킷 (낙상 통보 등, 페이로드 없음) — 스킵 금지, 즉시 처리 ──
        if (magic == kEvtMagic) {
            if (buffer.size() < (int)sizeof(dbj_evt_header_t))
                break;  // 헤더가 덜 옴 — 다음 readyRead 대기
            dbj_evt_header_t evt;
            memcpy(&evt, buffer.constData(), sizeof(evt));
            buffer.remove(0, sizeof(evt));

            if (evt.channel < kNumChannels) {
                // roi_id = 서버가 밝힌 발생 침대(=누구). 못 밝혔으면 kRoiIdNone(0xFF).
                // 유효 범위(침대 최대 개수) 판정은 침대 목록을 아는 화면 쪽이 한다.
                const int roiId = int(evt.roi_id);
                const float nx = evt.x / float(kRoiCoordScale);
                const float ny = evt.y / float(kRoiCoordScale);
                if (evt.type == kEvtFall)
                    emit fallEvent(evt.channel, roiId, evt.timestamp_ms, nx, ny);
                else if (evt.type == kEvtBedEgress)
                    emit bedEgressEvent(evt.channel, roiId, evt.timestamp_ms, nx, ny);
                // 웨어러블 생체데이터 이상 — x,y 는 쓰지 않는다(서버가 0 으로 채움)
                else if (evt.type == kEvtVitalAbnormal)
                    emit vitalAbnormalEvent(evt.channel, evt.timestamp_ms);
            }
            continue;
        }

        // ── 영상검색 결과 패킷 (🔍, 페이로드 있음) — 스킵 금지 ──
        if (magic == kSearchMagic) {
            if (buffer.size() < (int)sizeof(dbj_search_result_header_t))
                break;
            dbj_search_result_header_t sh;
            memcpy(&sh, buffer.constData(), sizeof(sh));

            const qint64 total = static_cast<qint64>(sizeof(sh)) + sh.text_len;
            if (buffer.size() < total)
                break;  // 텍스트가 아직 덜 옴 — 다음 readyRead 대기

            const QByteArray textBytes(buffer.constData() + sizeof(sh),
                                       static_cast<int>(sh.text_len));
            buffer.remove(0, static_cast<int>(total));

            emit searchResultReceived(sh.channel, QString::fromUtf8(textBytes));
            continue;
        }

        // ── 영상 프레임 패킷 ──
        // 1) 헤더 크기(16바이트)만큼도 안 모였으면 데이터 더 올 때까지 대기
        if (buffer.size() < (int)sizeof(dbj_vs_header_t))
            break;

        // 2) 헤더 영역 복사 (리틀엔디언 환경이므로 memcpy로 충분)
        dbj_vs_header_t header;
        memcpy(&header, buffer.constData(), sizeof(header));

        // 3) 매직넘버(0xDB4B) 검증, 다르면 스트림 어긋난 것.
        //    payload_len도 상한 검증 — 오염된 길이값을 믿으면 잘못된 메모리
        //    범위로 QImage를 만들거나 버퍼가 한없이 쌓인다.
        if (header.magic != 0xDB4B || header.payload_len > kMaxPayloadLen) {
            qDebug() << "⚠️ 스트림 어긋남! 연결을 끊고 재접속을 시도합니다."
                     << "(magic:" << Qt::hex << header.magic
                     << "payload_len:" << Qt::dec << header.payload_len << ")";
            buffer.clear();  // 오염된 버퍼 초기화
            sock->abort();   // 즉시 끊기 → UnconnectedState → 재접속 타이머 가동
            return;
        }

        // 4) 전체 패킷 크기 = 헤더(16B) + JPEG 크기 (qint64 — int 오버플로우 방지)
        const qint64 total = static_cast<qint64>(sizeof(header)) + header.payload_len;
        if (buffer.size() < total)
            break;  // JPEG 데이터가 아직 다 안 왔음

        // 5) 디코드는 뒤로 미루고, 채널별 "최신 프레임" 바이트만 보관한다.
        //    (여기서 디코드하면 backlog의 오래된 프레임까지 전부 처리하게 되어
        //     지연이 톱니처럼 쌓인다.)
        if (header.channel < kNumChannels) {
            latestJpeg[header.channel] = QByteArray(
                buffer.constData() + sizeof(header),
                static_cast<int>(header.payload_len));
            latestTs[header.channel] = header.timestamp_ms;
            hasFrame[header.channel] = true;
        }

        // 6) 사용이 끝난 패킷만큼 버퍼 맨 앞에서 도려내기
        buffer.remove(0, static_cast<int>(total));
    }

    // 7) 채널별 "가장 최신" 프레임만 디코드해서 내보낸다 (헛일 제거로 지연 최소화)
    for (int ch = 0; ch < kNumChannels; ++ch) {
        if (!hasFrame[ch] || suppressed_[ch])
            continue;
        QImage image = QImage::fromData(
            reinterpret_cast<const uchar*>(latestJpeg[ch].constData()),
            latestJpeg[ch].size(), "JPEG");
        if (image.isNull())
            continue;
        emit frameReceived(ch, image, static_cast<qint64>(latestTs[ch]));
    }
}

// ═══════════════════════════════════════════════════════════
//  제어 메시지 전송 (Qt → 서버)
// ═══════════════════════════════════════════════════════════
QByteArray StreamClient::ctrlHeader(uint8_t type, int ch, uint8_t pointCount, uint16_t reserved)
{
    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = type;
    h.channel = static_cast<uint8_t>(ch);
    h.point_count = pointCount;
    h.reserved = reserved;
    return QByteArray(reinterpret_cast<const char*>(&h), sizeof(h));
}

bool StreamClient::writeToServer(int idx, const QByteArray& pkt)
{
    QSslSocket* sock = socketForServer(idx);
    if (!sock || sock->state() != QAbstractSocket::ConnectedState)
        return false;
    sock->write(pkt);
    sock->flush();   // 채널마다 소켓이 다를 수 있어 즉시 flush
    return true;
}

bool StreamClient::writeToChannel(int ch, const QByteArray& pkt)
{
    if (ch < 0 || ch >= kNumChannels) return false;
    return writeToServer(serverForChannel(ch), pkt);
}

bool StreamClient::sendRoi(int ch, int roiId, const QPolygonF& normPts, bool clear)
{
    const int n = clear ? 0 : qMin(static_cast<int>(normPts.size()), 32);
    // reserved 하위 8비트가 침대 번호다 (프로토콜 DBJ_CTRL_ROI_ID 참조).
    // 삭제 시 kRoiIdAll을 실으면 그 채널의 침대를 전부 지운다.
    QByteArray pkt = ctrlHeader(clear ? kCtrlRoiClear : kCtrlRoiSet, ch,
                                static_cast<uint8_t>(n),
                                static_cast<uint16_t>(roiId & 0xFF));
    for (int i = 0; i < n; ++i) {
        dbj_roi_point_t p;
        p.x = static_cast<uint16_t>(
            qBound(0.0, normPts[i].x() * kRoiCoordScale, double(kRoiCoordScale)));
        p.y = static_cast<uint16_t>(
            qBound(0.0, normPts[i].y() * kRoiCoordScale, double(kRoiCoordScale)));
        pkt.append(reinterpret_cast<const char*>(&p), sizeof(p));
    }
    return writeToChannel(ch, pkt);
}

// "이 침대는 이 사람 자리" 를 서버에 통보한다.
// 서버는 이 매핑을 roi_zones에 남기고, 그 침대에서 생긴 추적 객체에 입소자를
// 귀속시켜 낙상·이탈 알림에 이름을 붙인다(server/core/identity_tracker.hpp).
bool StreamClient::sendRoiBind(int ch, int roiId, int residentId)
{
    QByteArray pkt = ctrlHeader(kCtrlRoiBind, ch, 0, 0);
    dbj_roi_bind_t b;
    b.roi_id = static_cast<uint8_t>(roiId & 0xFF);
    b.reserved = 0;
    b.resident_id = static_cast<uint32_t>(residentId > 0 ? residentId : 0);
    pkt.append(reinterpret_cast<const char*>(&b), sizeof(b));
    return writeToChannel(ch, pkt);
}

bool StreamClient::sendCameraSet(int ch, const QString& rtspUrl)
{
    const QByteArray url = rtspUrl.toUtf8();
    const int len = qMin(static_cast<int>(url.size()), kCameraUrlMax);
    QByteArray pkt = ctrlHeader(kCtrlCameraSet, ch, 0,
                                static_cast<uint16_t>(len));   // 이어지는 URL 바이트 길이
    pkt.append(url.constData(), len);
    return writeToChannel(ch, pkt);
}

bool StreamClient::sendCameraClear(int ch)
{
    return writeToChannel(ch, ctrlHeader(kCtrlCameraClear, ch, 0, 0));
}

// 카메라 이미지 파라미터 — 슬라이더 값(0~100)을 IMAGE_SET으로 담당 Pi에 보낸다.
// 실제 카메라 적용(ONVIF/SUNAPI)은 서버가 수행한다.
bool StreamClient::sendImageParams(int ch, int brightness, int contrast, int saturation)
{
    QByteArray pkt = ctrlHeader(kCtrlImageSet, ch, 0, sizeof(dbj_image_params_t));
    dbj_image_params_t ip;
    ip.brightness = static_cast<uint8_t>(qBound(0, brightness, 100));
    ip.contrast   = static_cast<uint8_t>(qBound(0, contrast, 100));
    ip.saturation = static_cast<uint8_t>(qBound(0, saturation, 100));
    pkt.append(reinterpret_cast<const char*>(&ip), sizeof(ip));
    const bool ok = writeToChannel(ch, pkt);
    // 어떤 채널이 어느 Pi로 나가는지 확인용 — CH2/3/4가 안 먹으면 서버 IMAGE_SET 핸들러 점검.
    if (ok)
        qDebug() << "➔ [Qt→서버] IMAGE_SET ch" << (ch + 1) << "(Pi" << serverForChannel(ch)
                 << ") b=" << brightness << "c=" << contrast << "s=" << saturation;
    return ok;
}

// 카메라 초점 — FOCUS_SET. 실제 적용(SUNAPI SimpleFocus)은 서버가 수행.
// area=false 전체 자동초점, true 클릭 지점 영역 초점.
bool StreamClient::sendFocus(int ch, bool area, float nx, float ny)
{
    QByteArray pkt = ctrlHeader(kCtrlFocusSet, ch, 0, sizeof(dbj_focus_t));
    dbj_focus_t f;
    f.mode = area ? kFocusArea : kFocusWhole;
    f.reserved = 0;
    f.x = static_cast<uint16_t>(qBound(0, int(nx * kRoiCoordScale), kRoiCoordScale));
    f.y = static_cast<uint16_t>(qBound(0, int(ny * kRoiCoordScale), kRoiCoordScale));
    pkt.append(reinterpret_cast<const char*>(&f), sizeof(f));
    return writeToChannel(ch, pkt);
}

// 경보 확인 — 서버의 PrivacyMasker를 깨워 다시 모자이크를 씌우게 한다(0x03).
bool StreamClient::sendAlarmConfirm(int ch)
{
    const bool ok = writeToChannel(ch, ctrlHeader(kCtrlAlarmConfirm, ch, 0, 0));
    if (ok)
        qDebug() << "🔓 [Qt -> 서버] 채널" << ch << "경보 확인 및 모자이크 복구 패킷 전송!";
    return ok;
}

// 환자 위험도 — 침대 단위(roiId), kRoiIdAll이면 채널 일괄.
bool StreamClient::sendRiskLevel(int ch, int roiId, int level)
{
    const bool ok = writeToChannel(
        ch, ctrlHeader(kCtrlRiskLevel, ch, static_cast<uint8_t>(level),
                       static_cast<uint16_t>(roiId & 0xFF)));  // 하위 8비트 = 침대 번호
    if (ok)
        qDebug() << "➔ [Qt -> 서버] 채널" << (ch + 1) << "침대"
                 << (roiId == kRoiIdAll ? -1 : roiId)
                 << "환자 위험도 변경 패킷 전송 완료 (값:" << level << ")";
    return ok;
}

bool StreamClient::sendSearchQuery(int ch, const QString& query)
{
    const QByteArray q = query.toUtf8();
    const int len = qMin(static_cast<int>(q.size()), kSearchQueryMax);
    QByteArray pkt = ctrlHeader(kCtrlSearchQuery, ch < 0 ? kChannelAll : ch, 0,
                                static_cast<uint16_t>(len));
    pkt.append(q.constData(), len);

    if (ch >= 0) return writeToChannel(ch, pkt);
    // 전체 채널 검색 — DB를 두 Pi가 공유하므로(2-Pi 분할) 아무 Pi에나 물어봐도
    // 전체 결과가 나온다. 연결된 소켓 중 먼저 찾은 것(Pi A 우선)으로 보낸다.
    for (int i = 0; i < kNumServers; ++i)
        if (writeToServer(i, pkt)) return true;
    return false;
}
