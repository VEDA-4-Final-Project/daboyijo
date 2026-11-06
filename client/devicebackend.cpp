#include "devicebackend.h"
#include "caredirectory.h"
#include "monitorbackend.h"
#include "mqttqtmanager.h"
#include "streamclient.h"
#include "streamprotocol.h"
#include "vitalsbackend.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>

namespace {

// 발견한 ONVIF 장비 1건.
struct DiscoveredCam {
    QString ip;
    QString model;
    QString mac;   // UUID에서 유도(대부분 카메라가 UUID에 MAC을 심음). 못 구하면 uuid.
    QString uuid;  // EndpointReference — 중복 제거 키
    bool onvif = false;  // ONVIF 장비인가(Windows WSD PC·프린터 응답 걸러내기)
};

// WS-Discovery Probe SOAP 메시지 (Types=NetworkVideoTransmitter → 카메라 대상).
QByteArray buildWsDiscoveryProbe() {
    const QString msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>uuid:%1</w:MessageID>"
        "<w:To e:mustUnderstand=\"true\">"
        "urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header>"
        // Types를 비워 모든 ONVIF 장비가 응답하게 한다(카메라만 걸러 못 뜨는 경우 방지).
        "<e:Body><d:Probe/></e:Body></e:Envelope>").arg(msgId).toUtf8();
}

// 이 PC가 붙어 있는 IPv4 네트워크 1개(인터페이스 + 주소 + 브로드캐스트 + prefix).
// 멀티캐스트·브로드캐스트·유니캐스트 스윕이 모두 이 목록을 공유한다.
struct LocalNet {
    QNetworkInterface iface;
    QHostAddress addr;
    QHostAddress bcast;
    int prefix = 0;
};

// 검색에 쓸 만한 인터페이스만 추린다(올라와 있고, 루프백이 아니고, IPv4 주소가 있는 것).
QList<LocalNet> usableIpv4Nets() {
    QList<LocalNet> out;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto f = iface.flags();
        if (!f.testFlag(QNetworkInterface::IsUp) ||
            !f.testFlag(QNetworkInterface::IsRunning) ||
            f.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry& e : iface.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            const int prefix = e.prefixLength();
            if (prefix <= 0 || prefix > 32) continue;   // prefix를 못 얻으면 계산 불가
            LocalNet n;
            n.iface  = iface;
            n.addr   = e.ip();
            n.prefix = prefix;
            n.bcast  = e.broadcast();
            if (n.bcast.isNull() && prefix < 32) {      // Qt가 안 채워주면 직접 계산
                const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
                n.bcast = QHostAddress((n.addr.toIPv4Address() & mask) | ~mask);
            }
            out.append(n);
        }
    }
    return out;
}

// Scopes 문자열에서 모델명 추출 (name 우선, hardware 보조, 그래도 없으면 모델형 토큰).
QString modelFromScopes(const QString& scopes) {
    QString name, hardware;
    const QStringList toks =
        scopes.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const QString& t : toks) {
        int i;
        if ((i = t.indexOf(QStringLiteral("/name/"), 0, Qt::CaseInsensitive)) >= 0)
            name = QUrl::fromPercentEncoding(t.mid(i + 6).toUtf8());
        else if ((i = t.indexOf(QStringLiteral("/hardware/"), 0, Qt::CaseInsensitive)) >= 0)
            hardware = QUrl::fromPercentEncoding(t.mid(i + 10).toUtf8());
    }
    if (!name.isEmpty() && !hardware.isEmpty())
        return name == hardware ? name
                                : name + QStringLiteral(" (") + hardware + QStringLiteral(")");
    if (!name.isEmpty()) return name;
    if (!hardware.isEmpty()) return hardware;

    // 폴백: name/hardware 스코프가 없는 장비 — 스코프 마지막 세그먼트 중 "모델처럼"
    // 생긴 토큰(대문자+숫자, 예: PNO-A9081R, XND-6080)을 찾아 표시한다.
    static const QRegularExpression modelLike(
        QStringLiteral("^[A-Z][A-Z0-9]*-?[A-Z0-9]{3,}$"));
    for (const QString& t : toks) {
        const QString seg =
            QUrl::fromPercentEncoding(t.mid(t.lastIndexOf('/') + 1).toUtf8());
        if (seg.contains(QRegularExpression(QStringLiteral("[0-9]"))) &&
            modelLike.match(seg).hasMatch())
            return seg;
    }
    return QStringLiteral("ONVIF 카메라");
}

// (MAC은 readyRead에서 arp를 비동기로 돌려 채운다 — 동기 조회는 응답 유실을 유발했다.)

// ProbeMatch 응답 1개 파싱 → DiscoveredCam.
DiscoveredCam parseProbeMatch(const QByteArray& datagram) {
    DiscoveredCam cam;
    QString xaddrs, scopes, types;
    QXmlStreamReader xml(datagram);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();  // 네임스페이스 접두어 제외 로컬명
            if (name == QStringLiteral("XAddrs"))       xaddrs = xml.readElementText();
            else if (name == QStringLiteral("Scopes"))  scopes = xml.readElementText();
            else if (name == QStringLiteral("Types"))   types  = xml.readElementText();
            else if (name == QStringLiteral("Address") && cam.uuid.isEmpty())
                cam.uuid = xml.readElementText().trimmed();
        }
    }
    // 브로드캐스트·유니캐스트 스윕을 쓰면 Windows WSD(PC·프린터)도 ProbeMatch를 보낸다.
    // ONVIF 장비만 표에 올린다 — 스코프/주소/타입 어디든 onvif 흔적이 있어야 한다.
    cam.onvif = scopes.contains(QStringLiteral("onvif"), Qt::CaseInsensitive) ||
                xaddrs.contains(QStringLiteral("/onvif/"), Qt::CaseInsensitive) ||
                types.contains(QStringLiteral("NetworkVideoTransmitter"), Qt::CaseInsensitive);
    if (!xaddrs.isEmpty()) {
        const QString first =
            xaddrs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).value(0);
        cam.ip = QUrl(first).host();
    }
    cam.model = modelFromScopes(scopes);
    // MAC은 호출부(readyRead)에서 ARP로 실제값을 채운다 — 여기선 uuid만 보관.
    return cam;
}

}  // namespace

namespace {
const char* kRoomNamesKey = "room/names";
}

DeviceBackend::DeviceBackend(CareDirectory* dir, StreamClient* stream, VitalsBackend* vitals, QObject* parent)
    : QObject(parent), dir_(dir), stream_(stream), vitals_(vitals)
{
    // 지속 저장된 연결 플래그 복원 (restoreCameraActive)
    const int mask = QSettings().value(QStringLiteral("camera/active_mask"), 0).toInt();
    for (int ch = 0; ch < 4; ++ch) active_[ch] = (mask & (1 << ch)) != 0;
    const QString ip = QSettings().value(QStringLiteral("camera/ip")).toString();
    for (int ch = 0; ch < 4; ++ch) ip_[ch] = active_[ch] ? ip : QString();

    // Pi 가 재접속하면 서버는 카메라를 모르므로 마지막 URL 을 다시 보낸다
    connect(stream_, &StreamClient::serverConnected, this, &DeviceBackend::resendForServer);
    connect(stream_, &StreamClient::connectionStateChanged, this, &DeviceBackend::refreshChannels);
    connect(dir_, &CareDirectory::zonesChanged, this, &DeviceBackend::channelsChanged);
    connect(dir_, &CareDirectory::changed, this, &DeviceBackend::channelsChanged);
    if (vitals_)
        connect(vitals_, &VitalsBackend::nodeOnlineChanged, this, [this](const QString& node, bool on) {
            online_[node] = on;
            emit alertStatusChanged();
        });

    sweepTimer_.setInterval(15);
    connect(&sweepTimer_, &QTimer::timeout, this, [this] {
        if (!sock_) { sweepTimer_.stop(); return; }
        const QByteArray probe = buildWsDiscoveryProbe();
        for (int i = 0; i < 24 && !sweep_.isEmpty(); ++i)
            sock_->writeDatagram(probe, QHostAddress(sweep_.takeFirst()), 3702);
        if (sweep_.isEmpty()) sweepTimer_.stop();
    });
    refreshChannels();
}

MqttQtManager* DeviceBackend::mqtt() const { return vitals_ ? vitals_->mqtt() : nullptr; }
QString DeviceBackend::savedIp() const { return QSettings().value(QStringLiteral("camera/ip")).toString(); }
QString DeviceBackend::savedUser() const { return QSettings().value(QStringLiteral("camera/user"), QStringLiteral("admin")).toString(); }

void DeviceBackend::refreshChannels()
{
    channels_.clear();
    for (int ch = 0; ch < 4; ++ch) {
        const bool server = stream_->isChannelConnected(ch);
        int beds = 0;
        for (const auto& z : dir_->zones(ch)) { Q_UNUSED(z); ++beds; }
        channels_ << QVariantMap{{"channel", ch}, {"active", active_[ch]}, {"server", server},
                                 {"ip", ip_[ch]}, {"beds", beds}};
    }
    emit channelsChanged();
}

void DeviceBackend::persistMask()
{
    int mask = 0;
    for (int ch = 0; ch < 4; ++ch) if (active_[ch]) mask |= (1 << ch);
    QSettings().setValue(QStringLiteral("camera/active_mask"), mask);
}

QString DeviceBackend::rtsp(const QString& ip, const QString& user, const QString& pw, int port, const QString& profile, int ch)
{
    return QStringLiteral("rtsp://%1:%2@%3:%4/%5/%6/media.smp").arg(user, pw, ip).arg(port).arg(ch).arg(profile);
}

QVariantMap DeviceBackend::connectCameras(const QString& ipIn, const QString& userIn, const QString& pw)
{
    const QString ip = ipIn.trimmed(), user = userIn.trimmed();
    if (ip.isEmpty()) return res(false, QStringLiteral("CCTV IP를 입력하거나 검색 목록에서 선택하세요."));
    const int port = 554;
    const QString prof = QStringLiteral("profile2");
    QSettings s;
    s.setValue(QStringLiteral("camera/ip"), ip);
    s.setValue(QStringLiteral("camera/user"), user);
    s.setValue(QStringLiteral("camera/port"), port);
    s.setValue(QStringLiteral("camera/profile"), prof);
    int sent = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const QString url = rtsp(ip, user, pw, port, prof, ch);
        lastUrl_[ch] = url;
        active_[ch] = true;
        ip_[ch] = ip;
        if (stream_->sendCameraSet(ch, url)) ++sent;
    }
    persistMask();
    refreshChannels();
    if (sent == 0) return res(false, QStringLiteral("영상 서버에 연결되어 있지 않습니다.\n서버 연결 상태를 확인하세요."));
    if (sent < 4) return res(true, QStringLiteral("4채널 중 %1채널만 전송했습니다.\n일부 Pi 서버가 끊겨 있습니다.").arg(sent));
    return res(true, QStringLiteral("4채널 연결 요청을 서버로 보냈습니다.\n서버가 카메라를 여는 동안 잠시 후 영상이 표시됩니다."));
}

bool DeviceBackend::anyCameraActive() const
{
    for (int ch = 0; ch < 4; ++ch) if (active_[ch]) return true;
    return false;
}

QString DeviceBackend::clearCameras()
{
    if (!anyCameraActive()) return QStringLiteral("연결된 카메라가 없습니다.");
    for (int ch = 0; ch < 4; ++ch) {
        stream_->sendCameraClear(ch);
        lastUrl_[ch].clear();
        active_[ch] = false;
        ip_[ch].clear();
    }
    persistMask();
    refreshChannels();
    return QString();
}

void DeviceBackend::resendForServer(int idx)
{
    for (int ch = 0; ch < 4; ++ch) {
        if (StreamClient::serverForChannel(ch) != idx || lastUrl_[ch].isEmpty()) continue;
        if (stream_->sendCameraSet(ch, lastUrl_[ch])) qDebug() << "Pi" << idx << "재접속 → ch" << ch << "카메라 자동 재전송";
    }
}

// ── ONVIF 검색 ───────────────────────────────────────────
void DeviceBackend::searchCameras(const QString& typedIp)
{
    if (!sock_) {
        sock_ = new QUdpSocket(this);
        connect(sock_, &QUdpSocket::readyRead, this, &DeviceBackend::onDatagrams);
    }
    if (sock_->state() != QAbstractSocket::BoundState &&
        !sock_->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress)) {
        searchStatus_ = QStringLiteral("검색용 소켓을 열지 못했습니다 — 네트워크 연결과 방화벽을 확인하세요.");
        emit foundChanged();
        return;
    }
    found_.clear();
    seen_.clear();
    sweep_.clear();
    searching_ = true;
    searchStatus_ = QStringLiteral("같은 망의 ONVIF 카메라를 검색 중…");
    emit foundChanged();

    const QHostAddress mcast(QStringLiteral("239.255.255.250"));
    QUdpSocket* sock = sock_;
    const QList<LocalNet> nets = usableIpv4Nets();
    auto sendProbes = [sock, mcast, nets] {
        const QByteArray probe = buildWsDiscoveryProbe();
        int sentOn = 0;
        for (const LocalNet& n : nets) {
            if (n.iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
                sock->setMulticastInterface(n.iface);
                if (sock->writeDatagram(probe, mcast, 3702) > 0) ++sentOn;
            }
            if (!n.bcast.isNull() && n.prefix < 32) sock->writeDatagram(probe, n.bcast, 3702);
        }
        if (sentOn == 0) sock->writeDatagram(probe, mcast, 3702);
    };
    sendProbes();
    QTimer::singleShot(700, this, sendProbes);
    QTimer::singleShot(1600, this, sendProbes);
    QTimer::singleShot(3000, this, sendProbes);

    QSet<quint32> queued;
    auto queueNet = [this](quint32 network, quint32 hosts) { for (quint32 i = 1; i < hosts - 1; ++i) sweep_.append(network + i); };
    for (const LocalNet& n : nets) {
        if (n.prefix < 23 || n.prefix > 30) continue;
        const quint32 mask = 0xFFFFFFFFu << (32 - n.prefix);
        const quint32 network = n.addr.toIPv4Address() & mask;
        if (queued.contains(network)) continue;
        queued.insert(network);
        queueNet(network, 1u << (32 - n.prefix));
    }
    const QHostAddress typed(typedIp.trimmed());
    if (!typed.isNull() && typed.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 network = typed.toIPv4Address() & 0xFFFFFF00u;
        if (!queued.contains(network)) { queued.insert(network); queueNet(network, 256); }
    }
    if (!sweep_.isEmpty()) sweepTimer_.start();

    QTimer::singleShot(7000, this, [this] {
        searching_ = false;
        const int n = found_.size();
        searchStatus_ = n > 0 ? QStringLiteral("검색 완료 — %1대 발견 (행을 클릭하면 IP가 채워집니다)").arg(n)
                              : QStringLiteral("검색 완료 — 카메라를 못 찾았습니다. 같은 공유기/스위치에 물려 있는지, 카메라 ONVIF 검색이 켜져 있는지, "
                                               "PC 방화벽이 Carenet의 UDP 수신을 막고 있지 않은지 확인하세요. 다른 대역이면 위 IP칸에 그 대역의 주소를 "
                                               "하나 적고 다시 검색하세요.");
        emit foundChanged();
    });
}

void DeviceBackend::onDatagrams()
{
    while (sock_->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize(int(sock_->pendingDatagramSize()));
        QHostAddress from;
        sock_->readDatagram(dg.data(), dg.size(), &from);
        const DiscoveredCam cam = parseProbeMatch(dg);
        if (cam.ip.isEmpty() || !cam.onvif) continue;
        const bool real = cam.model != QStringLiteral("ONVIF 카메라");
        if (seen_.contains(cam.ip)) {
            if (real)
                for (int i = 0; i < found_.size(); ++i) {
                    QVariantMap m = found_[i].toMap();
                    if (m["ip"].toString() == cam.ip && m["model"].toString() == QStringLiteral("ONVIF 카메라")) {
                        m["model"] = cam.model; found_[i] = m; emit foundChanged();
                    }
                }
            continue;
        }
        seen_.insert(cam.ip);
        found_ << QVariantMap{{"model", cam.model}, {"ip", cam.ip}, {"mac", QStringLiteral("…")}};
        emit foundChanged();

        const QString ip = cam.ip;
        auto* arp = new QProcess(this);
        connect(arp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, arp, ip](int, QProcess::ExitStatus) {
            const QString out = QString::fromLocal8Bit(arp->readAllStandardOutput());
            arp->deleteLater();
            static const QRegularExpression re(QStringLiteral("([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"));
            const auto m = re.match(out);
            const QString mac = m.hasMatch() ? m.captured(0).toUpper().replace('-', ':') : QStringLiteral("-");
            for (int i = 0; i < found_.size(); ++i) {
                QVariantMap v = found_[i].toMap();
                if (v["ip"].toString() == ip) { v["mac"] = mac; found_[i] = v; }
            }
            emit foundChanged();
        });
        arp->start(QStringLiteral("arp"), {QStringLiteral("-a"), ip});
    }
}

// ── 영상 조정 ────────────────────────────────────────────
QVariantMap DeviceBackend::imageParams(int ch) const
{
    QSettings st;
    const QString base = QStringLiteral("camera/image/ch%1/").arg(ch);
    return {{"bright", st.value(base + "bright", 50).toInt()},
            {"contrast", st.value(base + "contrast", 50).toInt()},
            {"saturation", st.value(base + "saturation", 50).toInt()}};
}

QVariantMap DeviceBackend::applyImage(int ch, int b, int c, int s)
{
    if (ch < 0 || ch >= 4) return res(false, QString());
    if (!stream_->sendImageParams(ch, b, c, s)) return res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
    QSettings st;
    const QString base = QStringLiteral("camera/image/ch%1/").arg(ch);
    st.setValue(base + "bright", b);
    st.setValue(base + "contrast", c);
    st.setValue(base + "saturation", s);
    return res(true, QString());
}

QVariantMap DeviceBackend::focusAll(int ch)
{
    return stream_->sendFocus(ch, false, 0.f, 0.f) ? res(true, QString())
                                                   : res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

QVariantMap DeviceBackend::focusAt(int ch, double nx, double ny)
{
    return stream_->sendFocus(ch, true, float(nx), float(ny)) ? res(true, QString())
                                                              : res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

// ── 침대(ROI) ────────────────────────────────────────────
QVariantList DeviceBackend::zones(int ch) const
{
    QVariantList list;
    if (ch < 0 || ch >= 4 || selectedRoom_ != 0) return list;
    for (const auto& z : dir_->zones(ch)) {
        QVariantList pts;
        for (const QPointF& p : z.poly) pts << p.x() << p.y();
        list << QVariantMap{{"id", z.id}, {"label", dir_->zoneLabel(ch, z.id)}, {"pts", pts}};
    }
    return list;
}

QVariantList DeviceBackend::beds(int ch) const
{
    QVariantList list;
    if (ch < 0 || ch >= 4 || selectedRoom_ != 0) return list;
    QVector<int> ids;
    for (const auto& z : dir_->zones(ch)) ids << z.id;
    std::sort(ids.begin(), ids.end());
    for (int id : ids) list << QVariantMap{{"id", id}, {"residentId", dir_->zoneResident(ch, id)}};
    return list;
}

QVariantList DeviceBackend::residentChoices(int ch) const
{
    QVariantList list{QVariantMap{{"id", 0}, {"name", QStringLiteral("미지정")}}};
    if (ch < 0 || ch >= 4) return list;
    for (int rid : dir_->residentsOn(ch)) {
        const QString n = dir_->resident(rid).name;
        list << QVariantMap{{"id", rid}, {"name", n.isEmpty() ? QStringLiteral("입소자 %1").arg(rid) : n}};
    }
    // 채널 밖 입소자에 묶여 있으면 그 항목도 보여 줘야 콤보가 거짓말을 안 한다
    for (const auto& z : dir_->zones(ch)) {
        const int bound = dir_->zoneResident(ch, z.id);
        bool have = false;
        for (const auto& v : list) if (v.toMap()["id"].toInt() == bound) have = true;
        if (bound > 0 && !have)
            list << QVariantMap{{"id", bound}, {"name", QStringLiteral("입소자 %1 (채널 밖)").arg(bound)}};
    }
    return list;
}

QVariantMap DeviceBackend::addZone(int ch, int roiId, const QVariantList& pts)
{
    QPolygonF poly;
    for (int i = 0; i + 1 < pts.size(); i += 2) poly << QPointF(pts[i].toDouble(), pts[i + 1].toDouble());
    const bool ok = stream_->sendRoi(ch, roiId, poly, false);
    dir_->setZone(ch, roiId, poly);
    refreshChannels();
    return ok ? res(true, QString()) : res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

QString DeviceBackend::removeBedPrompt(int ch, int roiId) const
{
    const int n = dir_->zones(ch).size();
    if (n == 0) return QString();
    if (roiId < 0) return QStringLiteral("채널 %1의 침대 %2개를 모두 제거할까요?").arg(ch + 1).arg(n);
    const QString who = dir_->zoneResidentName(ch, roiId);
    return QStringLiteral("채널 %1의 침대 %2(%3)를 제거할까요?").arg(ch + 1).arg(roiId + 1).arg(who.isEmpty() ? QStringLiteral("미지정") : who);
}

QVariantMap DeviceBackend::removeBed(int ch, int roiId)
{
    const bool all = roiId < 0;
    const bool ok = stream_->sendRoi(ch, all ? kRoiIdAll : roiId, QPolygonF(), true);
    if (all) {
        const auto copy = dir_->zones(ch);
        for (const auto& z : copy) dir_->removeZone(ch, z.id);
    } else dir_->removeZone(ch, roiId);
    refreshChannels();
    return ok ? res(true, QString()) : res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

QVariantMap DeviceBackend::bindBed(int ch, int roiId, int residentId)
{
    if (dir_->zoneResident(ch, roiId) == residentId) return res(true, QString());
    dir_->bindZone(ch, roiId, residentId);
    const bool ok = stream_->sendRoiBind(ch, roiId, residentId);
    return ok ? res(true, QString()) : res(false, QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

// ── 호실 ────────────────────────────────────────────────
QStringList DeviceBackend::rooms() const
{
    const QStringList saved = QSettings().value(QLatin1String(kRoomNamesKey)).toStringList();
    if (!saved.isEmpty()) return saved;
    return {CareDirectory::roomName(), QStringLiteral("102호")};
}

void DeviceBackend::setSelectedRoom(int r)
{
    if (r == selectedRoom_ || r < 0 || r >= rooms().size()) return;
    selectedRoom_ = r;
    emit roomsChanged();
    emit channelsChanged();
}

QString DeviceBackend::suggestRoom() const
{
    const QStringList r = rooms();
    static const QRegularExpression num(QStringLiteral("(\\d+)"));
    const auto m = num.match(r.isEmpty() ? QString() : r.last());
    if (!m.hasMatch()) return QString();
    return QString::number(m.captured(1).toInt() + 1) + r.last().mid(m.capturedEnd(1));
}

QString DeviceBackend::addRoom(const QString& nameIn)
{
    const QString name = nameIn.trimmed();
    if (name.isEmpty()) return QStringLiteral("호실 이름을 입력해 주세요.");
    QStringList r = rooms();
    if (r.contains(name)) return QStringLiteral("이미 있는 호실입니다.");
    r << name;
    QSettings().setValue(QLatin1String(kRoomNamesKey), r);
    emit roomsChanged();
    return QString();
}

void DeviceBackend::removeRoom(int idx)
{
    QStringList r = rooms();
    if (idx <= 0 || idx >= r.size()) return;
    r.removeAt(idx);
    QSettings().setValue(QLatin1String(kRoomNamesKey), r);
    if (selectedRoom_ >= r.size() || selectedRoom_ == idx) selectedRoom_ = 0;
    emit roomsChanged();
    emit channelsChanged();
}

// ── 알림 노드 ────────────────────────────────────────────
QString DeviceBackend::alertStatus() const
{
    const QString node = alertNodes().first();
    if (!online_.contains(node)) return QString();
    return online_.value(node) ? QStringLiteral("online") : QStringLiteral("offline");
}

QVariantMap DeviceBackend::alertSettings(const QString& node) const
{
    QSettings s;
    return {{"brightness", s.value(QStringLiteral("alarm/%1/brightness").arg(node), 70).toInt()},
            {"volume", s.value(QStringLiteral("alarm/%1/volume").arg(node), 60).toInt()}};
}

QString DeviceBackend::alertTestText() const { return QString::fromUtf8(MqttQtManager::kAlertTestText); }

bool DeviceBackend::testAlert(const QString& node, int b100, int vol)
{
    auto* m = mqtt();
    return m && m->sendAlarmTest(node, qRound(b100 * 255.0 / 100.0), vol);
}

bool DeviceBackend::applyAlert(const QString& node, int b100, int vol)
{
    auto* m = mqtt();
    if (!m || !m->sendAlarmConfig(node, qRound(b100 * 255.0 / 100.0), vol)) return false;
    QSettings s;
    s.setValue(QStringLiteral("alarm/%1/brightness").arg(node), b100);
    s.setValue(QStringLiteral("alarm/%1/volume").arg(node), vol);
    lastApplied_ = QStringLiteral("마지막 적용 %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    emit alertStatusChanged();
    return true;
}
