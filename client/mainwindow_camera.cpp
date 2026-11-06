// MainWindow — 장치 설정 — 카메라 연결·검색·이미지·초점, 알림 노드 설정, 방 관리
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  카메라 연결 — CCTV IP를 서버로 전송하면, 서버가 그 IP로 RTSP를 연다.
//  (Qt는 실제 CCTV에 직접 붙지 않는다. 붙는 대상은 항상 Pi 서버.)
// ═══════════════════════════════════════════════════════════

// ── ONVIF WS-Discovery 헬퍼 (같은 망 카메라 자동 탐색) ──────────
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

// 단일 CCTV IP → 채널별 RTSP URL. PNM-C16083RVQ(4센서 1대)는 IP 하나에 서브채널
// 0~3이 붙는다: rtsp://<계정>:<pw>@<IP>:<port>/<채널>/<profile>/media.smp
QString MainWindow::buildRtspUrl(const QString& ip, const QString& user,
                                 const QString& password, int port,
                                 const QString& profile, int channel)
{
    return QStringLiteral("rtsp://%1:%2@%3:%4/%5/%6/media.smp")
        .arg(user, password, ip)
        .arg(port)
        .arg(channel)
        .arg(profile);
}

bool MainWindow::sendCamera(int channel, const QString& rtspUrl)
{
    // 연결 안 됨이면 false — 호출자가 모아서 안내
    return stream_->sendCameraSet(channel, rtspUrl);
}

void MainWindow::sendCameraClear(int channel)
{
    stream_->sendCameraClear(channel);   // 연결 안 돼 있으면 조용히 무시
}

// ═══════════════════════════════════════════════════════════
//  카메라 이미지 파라미터 전송 — 슬라이더 값(0~100)을 IMAGE_SET 제어 메시지로
//  담당 Pi 서버에 보낸다. 실제 카메라 적용(ONVIF/SUNAPI)은 서버가 수행한다.
//  (sendCamera와 동일한 패턴 — 헤더 뒤에 4바이트 dbj_image_params_t를 붙임)
// ═══════════════════════════════════════════════════════════
// 채널별 이미지 값 저장/복원. 카메라 조회가 아니라 "이 PC가 마지막으로 보낸 값"이다.
void MainWindow::loadImageParams(int channel)
{
    if (channel < 0 || channel >= 4) return;
    if (!imgBright || !imgContrast || !imgSaturation) return;
    QSettings st;
    const QString base = QStringLiteral("camera/image/ch%1/").arg(channel);
    // 슬라이더를 프로그램이 움직이는 것뿐이라 신호를 막을 필요는 없다
    // (valueChanged 로 전송하지 않는다 — 전송은 [적용]에서만 한다).
    imgBright->setValue(st.value(base + QStringLiteral("bright"), 50).toInt());
    imgContrast->setValue(st.value(base + QStringLiteral("contrast"), 50).toInt());
    imgSaturation->setValue(st.value(base + QStringLiteral("saturation"), 50).toInt());
}

void MainWindow::saveImageParams(int channel)
{
    if (channel < 0 || channel >= 4) return;
    if (!imgBright || !imgContrast || !imgSaturation) return;
    QSettings st;
    const QString base = QStringLiteral("camera/image/ch%1/").arg(channel);
    st.setValue(base + QStringLiteral("bright"), imgBright->value());
    st.setValue(base + QStringLiteral("contrast"), imgContrast->value());
    st.setValue(base + QStringLiteral("saturation"), imgSaturation->value());
}

// 카메라가 안 붙은 채널에서는 ROI/이미지 조작이 아무 효과가 없다. 눌리는 것처럼
// 보이게 두면 관제사는 "적용했는데 안 바뀐다"고 판단한다 — 아예 못 누르게 하고
// 이유를 적는다. 침대 목록·입소자 매핑은 영상 없이도 손볼 수 있으므로 남겨 둔다.
void MainWindow::refreshCamControlsEnabled()
{
    const int ch = roiEditChannel;
    const bool on = (ch >= 0 && ch < 4) && cameraActive_[ch];

    for (ClickSlider* sl : {imgBright, imgContrast, imgSaturation})
        if (sl) sl->setEnabled(on);
    for (QPushButton* b : {imgApplyBtn, imgResetBtn, imgFocusBtn})
        if (b) b->setEnabled(on);
    if (imgDisabledHint) imgDisabledHint->setVisible(!on);

    // ROI는 "그리기"만 막는다 — 영상이 없으면 침대 모서리를 찍을 수 없다.
    if (roiButton) roiButton->setEnabled(on);
}

void MainWindow::sendImageParams(int channel, int b, int c, int s)
{
    if (!stream_->sendImageParams(channel, b, c, s))
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

// 카메라 초점 제어 — FOCUS_SET 제어 메시지로 담당 Pi에 전송. 실제 적용(SUNAPI
// SimpleFocus)은 서버가 수행. area=false 전체 자동초점, true 클릭 지점 영역 초점.
void MainWindow::sendFocus(int channel, bool area, float nx, float ny)
{
    if (!stream_->sendFocus(channel, area, nx, ny))
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

// 영상 타일의 호버 툴바 표시/숨김과 크롬 재배치를 맡는다.
// (초점 클릭은 WipeCompare::focusRequested 시그널로 따로 들어온다)
bool MainWindow::eventFilter(QObject* obj, QEvent* ev)
{
    // 영상 타일 호버 → 하단 툴바 표시/숨김. 버튼이 늘 떠 있으면 4분할 화면에서
    // 영상보다 버튼이 먼저 눈에 들어온다.
    if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave) {
        for (int ch = 0; ch < 4; ++ch) {
            if (obj != videoCards[ch] || !tileToolbars_[ch]) continue;
            bool show = (ev->type() == QEvent::Enter);
            if (!show) {
                // 커서가 아직 카드 안(툴바 버튼 위 등)이면 숨기지 않는다.
                QWidget* card = videoCards[ch];
                show = card->rect().contains(card->mapFromGlobal(QCursor::pos()));
            }
            tileToolbars_[ch]->setVisible(show);
            break;
        }
    }
    // 크롬은 레이아웃 밖에 놓인 자식이라 카드가 커지면 직접 따라가야 한다.
    if (ev->type() == QEvent::Resize) {
        for (int ch = 0; ch < 4; ++ch) {
            if (obj != videoCards[ch]) continue;
            layoutTileChrome(ch);
            break;
        }
    }

    return QMainWindow::eventFilter(obj, ev);
}

// "카메라 설정" 팝업의 "이미지" 탭 — 밝기/대비/채도 슬라이더 + Before/After
// 프리뷰. 채널 선택 후 [적용]을 누르면 값을 서버로 보내고(카메라에 실제 반영),
// 실시간(After) 뷰가 바뀌어 적용 전(Before) 스냅샷과 비교된다.
// (노출은 ONVIF에서 수동모드 전환이 필요해 야간감지에 위험 → 제외)
// 좌측 '이미지' 컨트롤 — 밝기/대비/채도 슬라이더 + 적용/초기화 + 전체 자동초점.
// Before/After 프리뷰는 우측 스테이지(buildCamStagePanel)가 담당한다. 대상 채널은
// 공용 채널(roiEditChannel)을 따른다.
QWidget* MainWindow::buildCamImagePage()
{
    auto* page = new QWidget();
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(12);

    auto* cap = new QLabel(QStringLiteral("밝기 · 대비 · 채도"));
    cap->setObjectName("camSectionCap");
    // 좌측 악센트 바 높이 = 라벨 높이다. 레이아웃이 남는 세로 공간을
    // 이 라벨에 나눠 주면 바가 글자보다 몇 배 길어진다 — 높이를 내용에 고정.
    cap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    col->addWidget(cap);

    // 슬라이더 3종 (ClickSlider — 값 숫자 표시 + 트랙 클릭 점프)
    imgBright     = new ClickSlider();
    imgContrast   = new ClickSlider();
    imgSaturation = new ClickSlider();
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("밝기"),   imgBright);
    form->addRow(QStringLiteral("대비"),   imgContrast);
    form->addRow(QStringLiteral("채도"),   imgSaturation);
    col->addLayout(form);

    // 값의 출처를 분명히 해 둔다 — 카메라에서 읽어온 현재값이 아니다.
    // 프로토콜에 조회(GET)가 없어서 "이 PC에서 마지막으로 보낸 값"만 알 수 있다.
    // 그 사실을 안 적어 두면 슬라이더가 카메라 상태를 보여준다고 오해한다.
    auto* srcHint = new QLabel(QStringLiteral(
        "값은 이 PC에 채널별로 기억됩니다. 카메라가 지금 실제로 어떤 값인지를 "
        "읽어오는 것은 아니므로, 다른 곳에서 바꿔을 때는 다를 수 있습니다."));
    srcHint->setObjectName("camHint");
    srcHint->setWordWrap(true);
    col->addWidget(srcHint);

    // 적용 / 초기화
    imgApplyBtn = new QPushButton(QStringLiteral("적용"));
    imgApplyBtn->setObjectName("camPrimary");   // 이 페이지의 주 액션
    imgApplyBtn->setCursor(Qt::PointingHandCursor);
    imgResetBtn = new QPushButton(QStringLiteral("초기화"));
    imgResetBtn->setObjectName("roiClear");
    imgResetBtn->setCursor(Qt::PointingHandCursor);
    auto* apply = imgApplyBtn;
    auto* reset = imgResetBtn;
    auto* br = new QHBoxLayout();
    br->addStretch();
    br->addWidget(reset);
    br->addWidget(apply);
    col->addLayout(br);

    // 포커스 구분선 캡션
    auto* fcap = new QLabel(QStringLiteral("초점"));
    fcap->setObjectName("camSectionCap");
    fcap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    col->addWidget(fcap);
    imgFocusBtn = new QPushButton(QStringLiteral("전체 자동초점"));
    imgFocusBtn->setObjectName("roiButton");
    imgFocusBtn->setCursor(Qt::PointingHandCursor);
    auto* afBtn = imgFocusBtn;
    col->addWidget(afBtn);
    auto* focusHint = new QLabel(
        QStringLiteral("💡 가운데 영상을 클릭하면 그 지점에 초점을 맞춥니다. "
                       "구분선을 좌우로 끌면 적용 전과 지금을 비교할 수 있습니다."));
    focusHint->setObjectName("camHint");
    focusHint->setWordWrap(true);
    col->addWidget(focusHint);

    // 미연결 채널에서 컨트롤이 회색인 이유를 적어 준다. 이유 없이 꺼져 있으면
    // "고장인가"로 읽히고, 관제사는 연결 탭으로 갈 생각을 못 한다.
    imgDisabledHint = new QLabel(QStringLiteral(
        "이 채널에 카메라가 연결되어 있지 않아 조절할 수 없습니다. "
        "위의 [연결] 탭에서 먼저 카메라를 붙여 주세요."));
    imgDisabledHint->setObjectName("camDisabledHint");
    imgDisabledHint->setWordWrap(true);
    imgDisabledHint->hide();
    col->addWidget(imgDisabledHint);

    connect(afBtn, &QPushButton::clicked, this, [this]() {
        sendFocus(roiEditChannel, false, 0.0f, 0.0f);
    });
    connect(apply, &QPushButton::clicked, this, [this]() {
        const int ch = roiEditChannel;
        // 적용 직전 현재 프레임을 Before 스냅샷으로 고정
        if (imgWipe_ && !lastFramePix_[ch].isNull())
            imgWipe_->setBefore(lastFramePix_[ch]);
        sendImageParams(ch, imgBright->value(), imgContrast->value(),
                        imgSaturation->value());
        saveImageParams(ch);   // 채널을 오갔다 돌아와도 같은 값이 보이게
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        for (ClickSlider* s : {imgBright, imgContrast, imgSaturation})
            s->setValue(50);
    });

    // 시작 채널의 저장값을 얹는다(없으면 50). 예전엔 무조건 50이라, 지난번에
    // 밝기를 70으로 올려 뒀어도 다시 들어오면 50으로 보이고 [적용]을 누르는
    // 순간 70→50으로 되돌아갔다.
    loadImageParams(roiEditChannel);
    return page;
}

// "카메라 설정" 팝업을 최초 1회 구성 — 카메라·ROI 작업을 팝업 안에서 직접 한다.
//   · 카메라 탭: 접속 정보 + [검색] → 결과표(팝업 내부에 채워짐) + [연결]/[해제]
//   · ROI 탭:   채널 선택 → 그 채널 영상을 팝업에 표시 → 그 위에 직접 ROI 그림
QWidget* MainWindow::buildCameraSettingsTab()
{
    if (cameraSettingsTab_) return cameraSettingsTab_;

    cameraSettingsTab_ = new QWidget();
    auto* page = cameraSettingsTab_;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    // 제목 + 한 줄 안내를 같은 줄에 — 제목만 덩그러니 있던 상단을 채운다.
    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("카메라 설정"));
    title->setObjectName("panelTitle");
    titleRow->addWidget(title);
    titleRow->addStretch();
    // 방을 먼저 고르고 그 안에서 모드를 고르는 순서라 왼쪽에 둔다.
    titleRow->addWidget(buildCamRoomSegment());
    titleRow->addSpacing(10);
    titleRow->addWidget(buildCamModeSegment());   // 모드는 페이지 전체를 바꾸므로 상단에
    outer->addLayout(titleRow);

    // 본문 3단: 채널 스트립(썸네일) │ 스테이지(큰 영상) │ 인스펙터(모드별 설정).
    // 영상 편집 도구의 표준 배치 — 왼쪽에서 대상을 고르고, 가운데를 보며, 오른쪽에서 만진다.
    auto* body = new QHBoxLayout();
    body->setSpacing(14);
    body->addWidget(buildCamChannelStrip(), 0);
    body->addWidget(buildCamStagePanel(), 1);
    body->addWidget(buildCamInspector(), 0);
    outer->addLayout(body, 1);

    selectCamChannel(0);
    setCamMode(QStringLiteral("연결"));
    refreshCamChannelStatus();
    return cameraSettingsTab_;
}

// 좌측 네비 "장치 설정" 페이지 — 상단 [카메라][알림] 세그먼트 + 서브탭 스택.
QWidget* MainWindow::buildDeviceSettingsTab()
{
    if (deviceSettingsTab_) return deviceSettingsTab_;

    deviceSettingsTab_ = new QWidget();
    auto* outer = new QVBoxLayout(deviceSettingsTab_);
    outer->setContentsMargins(18, 14, 18, 0);
    outer->setSpacing(10);

    auto* segRow = new QHBoxLayout();
    segRow->addWidget(buildDeviceModeSegment());
    segRow->addStretch();
    outer->addLayout(segRow);

    deviceStack_ = new QStackedWidget();
    deviceStack_->addWidget(buildCameraSettingsTab());  // 0: 카메라(연결/ROI/이미지)
    deviceStack_->addWidget(buildAlertSettingsTab());   // 1: 알림(밝기/음량/미리보기)
    outer->addWidget(deviceStack_, 1);
    return deviceSettingsTab_;
}

// [카메라][알림] 세그먼트 — 카메라 모드 세그먼트(camSeg/camSegBtn)와 같은 스타일 재사용.
QWidget* MainWindow::buildDeviceModeSegment()
{
    auto* segTrack = new QFrame();
    segTrack->setObjectName("camSeg");
    auto* seg = new QHBoxLayout(segTrack);
    seg->setContentsMargins(4, 4, 4, 4);
    seg->setSpacing(4);
    const QString modes[2] = {QStringLiteral("카메라"), QStringLiteral("알림")};
    for (int i = 0; i < 2; ++i) {
        deviceModeBtns_[i] = new QPushButton(modes[i]);
        deviceModeBtns_[i]->setObjectName("camSegBtn");
        deviceModeBtns_[i]->setCheckable(true);
        deviceModeBtns_[i]->setChecked(i == 0);
        deviceModeBtns_[i]->setAutoExclusive(true);   // 서로 배타 — 한쪽 누르면 반대쪽 해제
        deviceModeBtns_[i]->setCursor(Qt::PointingHandCursor);
        deviceModeBtns_[i]->setMinimumWidth(110);
        const int idx = i;
        connect(deviceModeBtns_[i], &QPushButton::clicked, this, [this, idx] {
            if (deviceStack_) deviceStack_->setCurrentIndex(idx);
        });
        seg->addWidget(deviceModeBtns_[i]);
    }
    return segTrack;
}

// 알림 노드 설정 서브탭 — 카메라 '이미지' 탭과 같은 3단 마스터-디테일 구성.
//   [왼쪽+가운데] LED 미리보기 스테이지  │  [오른쪽] 인스펙터(대상 노드 + 밝기/음량 + 테스트/적용)
// 값은 veda/alarm/control 로 나가고(mqtt->sendAlarmConfig/Test), 노드별로 QSettings 에 저장한다.
QWidget* MainWindow::buildAlertSettingsTab()
{
    auto* page = new QWidget();
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    // 제목 (카메라 설정 탭과 같은 자리)
    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("알림 설정"));
    title->setObjectName("panelTitle");
    titleRow->addWidget(title);
    titleRow->addStretch();
    outer->addLayout(titleRow);

    // 본문 2단: [ 미리보기 스테이지(왼쪽+가운데, stretch) ] │ [ 인스펙터(오른쪽) ]
    auto* body = new QHBoxLayout();
    body->setSpacing(14);

    // (A) 미리보기 스테이지 — 카메라 스테이지(#camStage)와 같은 카드에 얹는다.
    auto* stageCard = new QFrame();
    stageCard->setObjectName("camStage");
    auto* sv = new QVBoxLayout(stageCard);
    sv->setContentsMargins(12, 10, 12, 12);
    sv->setSpacing(8);
    auto* stageCap = new QLabel(QStringLiteral("LED 미리보기 (64×32)"));
    stageCap->setObjectName("camStageCap");
    sv->addWidget(stageCap, 0, Qt::AlignHCenter);
    alertPreview_ = new AlertMatrixPreview();
    // 미리보기 문구 = "테스트" 때 노드 LED 에 실제로 뜨는 문구와 동일하게(한 상수에서).
    alertPreview_->setText(QString::fromUtf8(MqttQtManager::kAlertTestText));
    alertPreview_->setMinimumHeight(240);
    alertPreview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sv->addWidget(alertPreview_, 1);
    body->addWidget(stageCard, 1);

    // (B) 인스펙터 — 카메라 인스펙터(#camControlPanel)와 같은 카드.
    auto* panel = new QFrame();
    panel->setObjectName("camControlPanel");
    panel->setFixedWidth(380);
    auto* cl = new QVBoxLayout(panel);
    cl->setContentsMargins(18, 16, 18, 18);
    cl->setSpacing(14);

    // 헤더 — 대상 노드 + 온라인 배지(카메라 인스펙터의 연결 배지와 같은 스타일)
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    auto* headTitle = new QLabel(QStringLiteral("대상 노드"));
    headTitle->setObjectName("camInspCh");
    alertStatusBadge_ = new QLabel(QStringLiteral("상태 미확인"));
    // 카메라 인스펙터 연결 배지(camInspPill, #camPill)와 objectName을 공유하면
    // 서로 다른 속성 어휘(이쪽은 severity, 저쪽은 02-04의 연결 상태)가 섞인다
    // — 02-03 정정 C로 분리.
    alertStatusBadge_->setObjectName("alertNodeBadge");
    head->addWidget(headTitle);
    head->addWidget(alertStatusBadge_);
    head->addStretch();
    cl->addLayout(head);

    // 이벤트 필터(filterEventType)와 같은 어두운 테마 드롭다운으로 통일 — 기본
    // QComboBox 는 이 컨테이너 안에서 스코프된 스타일을 못 받아 흰 배경으로 떴었다.
    alertNode_ = new QComboBox();
    alertNode_->setObjectName("formEdit");
    alertNode_->addItem(QStringLiteral("alarm_rpi_01"));   // 정적 목록(추후 확장)
    cl->addWidget(alertNode_);

    auto* rule = new QFrame();
    rule->setObjectName("camRule");
    rule->setFixedHeight(1);
    cl->addWidget(rule);

    // 밝기 · 음량 (섹션 캡션 + 폼 — 카메라 이미지 탭과 동일)
    auto* ctrlCap = new QLabel(QStringLiteral("밝기 · 음량"));
    ctrlCap->setObjectName("camSectionCap");
    ctrlCap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    cl->addWidget(ctrlCap);

    alertBright_ = new ClickSlider();
    alertVol_    = new ClickSlider();
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("LED 밝기"),   alertBright_);
    form->addRow(QStringLiteral("스피커 음량"), alertVol_);
    cl->addLayout(form);

    // 테스트 / 적용 — 우측 정렬(보조 + 주 액션)
    auto* testBtn = new QPushButton(QStringLiteral("테스트"));
    testBtn->setObjectName("roiClear");
    testBtn->setCursor(Qt::PointingHandCursor);
    auto* applyBtn = new QPushButton(QStringLiteral("적용"));
    applyBtn->setObjectName("camPrimary");
    applyBtn->setCursor(Qt::PointingHandCursor);
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(testBtn);
    btnRow->addWidget(applyBtn);
    cl->addLayout(btnRow);

    // 카메라 이미지 탭의 초점 안내처럼 — 한 문장씩, 짧게.
    auto* testHint = new QLabel(QStringLiteral("💡 테스트는 저장하지 않고 지금 값으로 잠깐 보여줍니다."));
    testHint->setObjectName("camHint");
    testHint->setWordWrap(true);
    cl->addWidget(testHint);
    auto* applyHint = new QLabel(QStringLiteral("💡 적용은 지금 값을 노드에 바로 반영하고 저장합니다."));
    applyHint->setObjectName("camHint");
    applyHint->setWordWrap(true);
    cl->addWidget(applyHint);

    cl->addStretch();   // 내용은 위에서부터 — 카드는 스테이지 높이를 따라간다

    alertApplied_ = new QLabel(QStringLiteral("마지막 적용 --:--:--"));
    alertApplied_->setObjectName("camHint");
    cl->addWidget(alertApplied_);

    body->addWidget(panel, 0);
    outer->addLayout(body, 1);

    // ── 배선 ──
    // 미리보기 밝기 매핑 — 모니터에서 저조도(슬라이더 0~30%)가 새까맣게 보이던 문제 보정.
    // 슬라이더 0~100 을 미리보기 [76,255] 로 띄운다(최저도 슬라이더 30 수준으로 보이게).
    // 실제 노드로 보내는 밝기는 sendAlarm* 에서 슬라이더 그대로 0~255 로 선형 전송.
    auto toPreviewB = [](int v) {
        return int(qRound(76.0 + (255.0 - 76.0) * v / 100.0));
    };

    connect(alertBright_, &QSlider::valueChanged, this, [this, toPreviewB](int v) {
        if (alertPreview_) alertPreview_->setBrightness(toPreviewB(v));
    });

    // 노드별 저장값 로드 + 온라인 배지 갱신 (기본: 밝기 70 / 음량 60)
    auto loadForNode = [this, toPreviewB](const QString& node) {
        QSettings s;
        const int b   = s.value(QStringLiteral("alarm/%1/brightness").arg(node), 70).toInt();
        const int vol = s.value(QStringLiteral("alarm/%1/volume").arg(node), 60).toInt();
        if (alertBright_) alertBright_->setValue(b);
        if (alertVol_)    alertVol_->setValue(vol);
        if (alertPreview_) alertPreview_->setBrightness(toPreviewB(b));
        refreshAlertStatusBadge();
    };
    connect(alertNode_, &QComboBox::currentTextChanged, this,
            [loadForNode](const QString& n) { loadForNode(n); });

    // 테스트 — 현재 값으로 노드에 실제 명령(성공 시 별도 토스트 없음: 현장에서 결과 확인)
    connect(testBtn, &QPushButton::clicked, this, [this] {
        if (!mqtt) return;
        const QString node = alertNode_->currentText();
        const int b255 = qRound(alertBright_->value() * 255.0 / 100.0);
        mqtt->sendAlarmTest(node, b255, alertVol_->value());   // 실패 시 onMqttError 로 안내
    });

    // 적용 — 노드에 설정 전송 + 성공 시 QSettings 저장 + 시각 갱신
    connect(applyBtn, &QPushButton::clicked, this, [this] {
        if (!mqtt) return;
        const QString node = alertNode_->currentText();
        const int b255 = qRound(alertBright_->value() * 255.0 / 100.0);
        if (mqtt->sendAlarmConfig(node, b255, alertVol_->value())) {
            QSettings s;
            s.setValue(QStringLiteral("alarm/%1/brightness").arg(node), alertBright_->value());
            s.setValue(QStringLiteral("alarm/%1/volume").arg(node), alertVol_->value());
            if (alertApplied_)
                alertApplied_->setText(QStringLiteral("마지막 적용 %1")
                                           .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
        }
    });

    loadForNode(alertNode_->currentText());   // 초기 로드
    return page;
}

// 상단 페이지 모드 세그먼트 — 트랙 위에 얹힌 알약 버튼 3개.
// 상단 방 세그먼트(101호 / 102호 …). 실시간 관제 트리와 selectedRoom_을 공유하므로
// 여기서 방을 바꾸면 관제 화면도 같은 방을 보게 된다 — 두 화면이 서로 다른 방을
// 가리키면 "지금 어느 방을 설정 중인가"가 흐려진다.
QWidget* MainWindow::buildCamRoomSegment()
{
    camRoomSeg_ = new QFrame();
    camRoomSeg_->setObjectName("camSeg");
    auto* seg = new QHBoxLayout(camRoomSeg_);
    seg->setContentsMargins(4, 4, 4, 4);
    seg->setSpacing(4);
    rebuildCamRoomSegment();
    return camRoomSeg_;
}

// 방 목록이 바뀔 때마다 버튼을 통째로 다시 만든다. 방 수가 가변이라 버튼을
// 미리 만들어 둘 수 없다.
void MainWindow::rebuildCamRoomSegment()
{
    if (!camRoomSeg_) return;
    auto* seg = qobject_cast<QHBoxLayout*>(camRoomSeg_->layout());
    if (!seg) return;
    while (QLayoutItem* it = seg->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    camRoomBtns_.clear();

    const QStringList rooms = roomNames();
    for (int r = 0; r < rooms.size(); ++r) {
        auto* b = new QPushButton(rooms.at(r));
        b->setObjectName("camSegBtn");
        b->setCheckable(true);
        b->setChecked(r == selectedRoom_);
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumWidth(76);
        if (r != 0) {
            b->setToolTip(QStringLiteral("카메라가 배정되지 않은 방입니다\n"
                                         "우클릭하면 이 방 자리를 지울 수 있습니다"));
            // 삭제는 우클릭에 둔다 — 버튼마다 ×를 붙이면 세그먼트가 지저분해지고,
            // 자주 쓰는 동작도 아니다.
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QPushButton::customContextMenuRequested, this,
                    [this, b, r](const QPoint& pos) {
                        QMenu menu(this);
                        QAction* del = menu.addAction(QStringLiteral("이 방 자리 삭제"));
                        if (menu.exec(b->mapToGlobal(pos)) == del) removeRoom(r);
                    });
        } else {
            b->setToolTip(QStringLiteral("카메라가 배정된 방입니다"));
        }
        connect(b, &QPushButton::clicked, this, [this, r] { selectRoom(r); });
        camRoomBtns_.append(b);
        seg->addWidget(b);
    }

    // [+] 호실 추가 — 방 "자리"만 늘린다(카메라는 따라오지 않는다).
    auto* add = new QPushButton(QStringLiteral("＋"));
    add->setObjectName("camSegBtn");
    add->setCursor(Qt::PointingHandCursor);
    add->setFixedWidth(34);
    add->setToolTip(QStringLiteral("호실 추가 — 방 자리를 만들어 둡니다"));
    connect(add, &QPushButton::clicked, this, &MainWindow::onAddRoomClicked);
    seg->addWidget(add);
}

// [+] — 다음 호실 번호를 추정해 기본값으로 제안한다.
void MainWindow::onAddRoomClicked()
{
    QStringList rooms = roomNames();

    // 마지막 방 이름에서 숫자를 뽑아 +1 (예: "102호" → "103호"). 숫자가 없으면 빈 제안.
    QString suggestion;
    static const QRegularExpression num(QStringLiteral("(\\d+)"));
    const auto m = num.match(rooms.isEmpty() ? QString() : rooms.last());
    if (m.hasMatch()) {
        const QString tail = rooms.last().mid(m.capturedEnd(1));
        suggestion = QString::number(m.captured(1).toInt() + 1) + tail;
    }

    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("호실 추가"),
                              QStringLiteral("추가할 호실 이름"),
                              QLineEdit::Normal, suggestion, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (rooms.contains(name)) {
        QMessageBox::information(this, QStringLiteral("호실 추가"),
                                 QStringLiteral("이미 있는 호실입니다."));
        return;
    }

    rooms << name;
    QSettings st;
    st.setValue(QLatin1String(kSettingsRoomNames), rooms);
    rebuildCamRoomSegment();
    rebuildResourceRooms();
    refreshResourceTree();
}

// 방 자리 삭제. 0번(실제 카메라가 붙은 방)은 지울 수 없다 — 지우면 영상월이
// 가리킬 대상이 사라진다.
void MainWindow::removeRoom(int room)
{
    QStringList rooms = roomNames();
    if (room <= 0 || room >= rooms.size()) return;

    const QString name = rooms.at(room);
    if (QMessageBox::question(
            this, QStringLiteral("호실 삭제"),
            QStringLiteral("%1 자리를 지울까요?").arg(name)) != QMessageBox::Yes)
        return;

    rooms.removeAt(room);
    QSettings st;
    st.setValue(QLatin1String(kSettingsRoomNames), rooms);

    // 보고 있던 방을 지웠거나 뒤 번호가 당겨졌으면 실카메라 방으로 되돌린다.
    if (selectedRoom_ >= rooms.size() || selectedRoom_ == room) {
        selectedRoom_ = 0;
        applyRoomView();
        for (int ch = 0; ch < 4; ++ch) refreshRoiZones(ch);
    }
    rebuildCamRoomSegment();
    rebuildResourceRooms();
    refreshResourceTree();
}

QWidget* MainWindow::buildCamModeSegment()
{
    auto* segTrack = new QFrame();
    segTrack->setObjectName("camSeg");
    auto* seg = new QHBoxLayout(segTrack);
    seg->setContentsMargins(4, 4, 4, 4);
    seg->setSpacing(4);
    const QString modes[3] = {QStringLiteral("연결"), QStringLiteral("ROI"),
                              QStringLiteral("이미지")};
    for (int i = 0; i < 3; ++i) {
        camModeBtns[i] = new QPushButton(modes[i]);
        camModeBtns[i]->setObjectName("camSegBtn");
        camModeBtns[i]->setCheckable(true);
        camModeBtns[i]->setChecked(i == 0);
        camModeBtns[i]->setCursor(Qt::PointingHandCursor);
        camModeBtns[i]->setMinimumWidth(96);
        const QString m = modes[i];
        connect(camModeBtns[i], &QPushButton::clicked, this, [this, m] { setCamMode(m); });
        seg->addWidget(camModeBtns[i]);
    }
    return segTrack;
}

// 좌측 채널 스트립 — 채널마다 라이브 썸네일 타일. 한 채널을 설정하는 동안에도
// 나머지 채널이 지금 무엇을 비추고 있는지 계속 보인다(예전 텍스트 버튼 4개와의 차이).
QWidget* MainWindow::buildCamChannelStrip()
{
    auto* strip = new QFrame();
    strip->setObjectName("camStrip");
    strip->setFixedWidth(196);
    auto* v = new QVBoxLayout(strip);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    auto* cap = new QLabel(QStringLiteral("채널"));
    cap->setObjectName("camRailCap");
    v->addWidget(cap);

    for (int i = 0; i < 4; ++i) {
        // 타일 자체가 체크 버튼 — 썸네일·라벨은 자식이라 클릭이 버튼으로 전달된다.
        camChannelBtns[i] = new QPushButton();
        camChannelBtns[i]->setObjectName("camTile");
        camChannelBtns[i]->setCheckable(true);
        camChannelBtns[i]->setChecked(i == 0);
        camChannelBtns[i]->setCursor(Qt::PointingHandCursor);
        // QPushButton::sizeHint는 자식 레이아웃을 보지 않고 텍스트로만 계산한다.
        // 타일엔 텍스트가 없으니 높이를 직접 못박지 않으면 버튼이 한 줄 높이로
        // 잡혀 안의 썸네일이 잘린다.
        camChannelBtns[i]->setFixedHeight(126);

        auto* tv = new QVBoxLayout(camChannelBtns[i]);
        tv->setContentsMargins(7, 7, 7, 7);
        tv->setSpacing(6);

        // 썸네일이 남는 높이를 먹는다(라벨 줄은 자기 높이만 차지).
        camThumbs[i] = new FramePreview(QStringLiteral("신호 없음"));
        camThumbs[i]->setMinimumHeight(56);
        camThumbs[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        camThumbs[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        tv->addWidget(camThumbs[i], 1);

        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        auto* chLab = new QLabel(QStringLiteral("CH %1").arg(i + 1));
        chLab->setObjectName("camTileCh");
        camChannelStatus[i] = new QLabel(QStringLiteral("○ 미연결"));
        camChannelStatus[i]->setObjectName("camChStatus");
        camChannelStatus[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // 자식 위젯이 클릭을 먹지 않게 — 타일 어디를 눌러도 채널이 선택되도록.
        chLab->setAttribute(Qt::WA_TransparentForMouseEvents);
        camChannelStatus[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(chLab);
        row->addStretch();
        row->addWidget(camChannelStatus[i]);
        tv->addLayout(row);

        const int ch = i;
        connect(camChannelBtns[i], &QPushButton::clicked, this,
                [this, ch] { selectCamChannel(ch); });
        v->addWidget(camChannelBtns[i]);
    }
    v->addStretch();
    return strip;
}

// 우측 인스펙터 — 헤더(현재 채널·상태·주소) + 모드별 페이지 스택.
QWidget* MainWindow::buildCamInspector()
{
    auto* panel = new QFrame();
    panel->setObjectName("camControlPanel");
    panel->setFixedWidth(380);
    auto* cl = new QVBoxLayout(panel);
    cl->setContentsMargins(18, 16, 18, 18);
    cl->setSpacing(14);

    // 헤더 — "지금 무엇을 만지고 있는지"를 인스펙터 안에서 항상 알 수 있게.
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    camInspCh = new QLabel(QStringLiteral("CH 1"));
    camInspCh->setObjectName("camInspCh");
    camInspPill = new QLabel(QStringLiteral("미연결"));
    camInspPill->setObjectName("camPill");
    head->addWidget(camInspCh);
    head->addWidget(camInspPill);
    head->addStretch();
    cl->addLayout(head);

    camInspIp = new QLabel(QStringLiteral("카메라가 연결되지 않았습니다"));
    camInspIp->setObjectName("camInspIp");
    cl->addWidget(camInspIp);

    auto* rule = new QFrame();
    rule->setObjectName("camRule");
    rule->setFixedHeight(1);
    cl->addWidget(rule);

    camControlStack = new QStackedWidget();
    camControlStack->addWidget(buildCamConnectPage());
    camControlStack->addWidget(buildCamRoiPage());
    camControlStack->addWidget(buildCamImagePage());
    // 스택이 '현재 페이지' 높이에 맞게 줄어들도록 — 숨은 페이지는 세로 크기 계산에서 제외.
    auto sizeToCurrent = [](QStackedWidget* st) {
        for (int i = 0; i < st->count(); ++i) {
            auto pol = st->widget(i)->sizePolicy();
            pol.setVerticalPolicy(i == st->currentIndex() ? QSizePolicy::Preferred
                                                          : QSizePolicy::Ignored);
            st->widget(i)->setSizePolicy(pol);
        }
    };
    connect(camControlStack, &QStackedWidget::currentChanged, this,
            [this, sizeToCurrent](int) { sizeToCurrent(camControlStack); });
    sizeToCurrent(camControlStack);
    cl->addWidget(camControlStack, 0);
    cl->addStretch();   // 내용은 위에서부터 — 카드는 스테이지 높이를 따라간다
    return panel;
}

// 좌측 '연결' 페이지 — 접속 폼 + 검색/연결/해제 + 검색 결과 표.
QWidget* MainWindow::buildCamConnectPage()
{
    auto* camTab = new QWidget();
    auto* camV = new QVBoxLayout(camTab);
    camV->setContentsMargins(0, 0, 0, 0);
    camV->setSpacing(12);

    auto* cap = new QLabel(QStringLiteral("카메라 접속"));
    cap->setObjectName("camSectionCap");
    cap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    camV->addWidget(cap);

    // 이 페이지에서만 채널 선택이 의미가 없다는 걸 분명히 해 둔다.
    // PNM-C16083RVQ는 센서 4개가 한 몸이라 IP도 하나고, 4채널이 함께 붙고 함께
    // 끊긴다. 예전엔 좌측에서 CH2를 골라 놓고 [해제]를 누르면 방 전체가 꺼졌는데,
    // 화면 어디에도 그렇게 된다는 말이 없었다.
    auto* bundleHint = new QLabel(QStringLiteral(
        "이 카메라는 센서 4개가 한 몸입니다 — IP 하나로 4채널이 "
        "함께 연결되고 함께 해제됩니다. 왼쪽 채널 선택은 ROI·이미지 "
        "탭에서만 적용됩니다."));
    bundleHint->setObjectName("camHint");
    bundleHint->setWordWrap(true);
    camV->addWidget(bundleHint);

    // 접속 정보 폼 (마지막 값 복원)
    QSettings s;
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    camIpEdit = new QLineEdit(s.value(QStringLiteral("camera/ip")).toString());
    camIpEdit->setPlaceholderText(
        QStringLiteral("예: 172.20.32.31  (아래 검색 결과를 클릭하면 자동 입력)"));
    camUserEdit = new QLineEdit(
        s.value(QStringLiteral("camera/user"), QStringLiteral("admin")).toString());
    camPwEdit = new QLineEdit();
    camPwEdit->setEchoMode(QLineEdit::Password);
    camPwEdit->setPlaceholderText(QStringLiteral("CCTV 비밀번호"));
    // 포트(554)·프로파일(profile2)은 고정 — 입력받지 않는다.
    // 다크 스타일 적용 — 스타일시트가 objectName "formEdit"인 입력칸만 칠한다.
    for (QLineEdit* e : {camIpEdit, camUserEdit, camPwEdit})
        e->setObjectName(QStringLiteral("formEdit"));
    form->addRow(QStringLiteral("CCTV IP"), camIpEdit);
    form->addRow(QStringLiteral("계정"), camUserEdit);
    form->addRow(QStringLiteral("비밀번호"), camPwEdit);
    camV->addLayout(form);

    // 액션 버튼 — 두 줄. 인스펙터 폭(약 380px)에 검색·연결·해제 셋을 한 줄로
    // 넣으면 글자가 잘린다. 검색은 성격이 다른(탐색) 동작이라 위로 뺐다.
    searchCameraButton = new QPushButton(QStringLiteral("\U0001F50D 같은 망 카메라 검색"));
    searchCameraButton->setObjectName("roiButton");
    searchCameraButton->setCursor(Qt::PointingHandCursor);
    connect(searchCameraButton, &QPushButton::clicked, this, &MainWindow::onSearchCameraClicked);
    camV->addWidget(searchCameraButton);

    addCameraButton = new QPushButton(QStringLiteral("\U0001F4F7 4채널 연결"));
    addCameraButton->setObjectName("camPrimary");   // 이 페이지의 주 액션
    addCameraButton->setCursor(Qt::PointingHandCursor);
    addCameraButton->setToolTip(QStringLiteral("입력한 IP로 4채널을 한꺼번에 연결합니다"));
    connect(addCameraButton, &QPushButton::clicked, this, &MainWindow::onAddCameraClicked);

    clearCameraButton = new QPushButton(QStringLiteral("전체 해제"));
    clearCameraButton->setObjectName("roiClear");
    clearCameraButton->setCursor(Qt::PointingHandCursor);
    clearCameraButton->setToolTip(QStringLiteral("4채널을 한꺼번에 해제합니다"));
    connect(clearCameraButton, &QPushButton::clicked, this, &MainWindow::onCameraClearClicked);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);
    btnRow->addWidget(addCameraButton, 2);   // 주 액션이 더 넓게
    btnRow->addWidget(clearCameraButton, 1);
    camV->addLayout(btnRow);

    // 검색 결과 표 (팝업 내부에 인라인으로 채워진다 — 별도 창 안 띄움)
    discoveryStatus = new QLabel(
        QStringLiteral("‘검색’을 누르면 같은 망의 카메라가 아래에 나타납니다. 행을 클릭하면 IP가 채워져요."));
    discoveryStatus->setObjectName("camHint");   // 문장이라 자간 넓은 캡션 스타일은 안 맞음
    discoveryStatus->setWordWrap(true);
    camV->addWidget(discoveryStatus);

    discoveryTable = new QTableWidget(0, 3);
    discoveryTable->setObjectName(QStringLiteral("logTable"));  // 다크 표 스타일 재사용
    discoveryTable->setHorizontalHeaderLabels(
        {QStringLiteral("모델"), QStringLiteral("IP"), QStringLiteral("MAC / ID")});
    discoveryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    discoveryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    discoveryTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    discoveryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    discoveryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    discoveryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    discoveryTable->verticalHeader()->setVisible(false);
    // 표 높이는 행 수에 맞춘다(syncDiscoveryTableHeight) — 결과가 없으면 아예 숨긴다.
    // 빈 표를 늘 띄워두면 '연결' 카드 아래에 큰 빈 공간이 남는다.
    discoveryTable->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // 행 클릭/더블클릭 → IP 입력칸에 채워준다.
    auto fillIpFromRow = [this](int r, int) {
        if (discoveryTable->item(r, 1)) camIpEdit->setText(discoveryTable->item(r, 1)->text());
        // 다른 카메라를 고르면 이전 카메라의 계정·비번은 남기지 않고 초기화.
        camUserEdit->setText(QStringLiteral("admin"));
        camPwEdit->clear();
    };
    connect(discoveryTable, &QTableWidget::cellClicked, this, fillIpFromRow);
    connect(discoveryTable, &QTableWidget::cellDoubleClicked, this, fillIpFromRow);
    camV->addWidget(discoveryTable, 0);
    syncDiscoveryTableHeight();   // 0행 → 숨김 상태로 시작

    // 검색용 UDP 소켓 (1회 생성·재사용). 응답이 오면 표에 인라인으로 추가.
    // 임의 포트에 바인드 → 카메라는 우리가 보낸 소스 포트로 유니캐스트 ProbeMatch를
    // 돌려준다. (3702 공유 바인드는 Windows에서 유니캐스트 응답을 다른 프로세스가
    // 가로채 오히려 아무것도 못 받으므로 쓰지 않는다.)
    discoverySocket = new QUdpSocket(camTab);
    discoverySocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);
    connect(discoverySocket, &QUdpSocket::readyRead, this, [this]() {
        while (discoverySocket->hasPendingDatagrams()) {
            QByteArray dg;
            dg.resize(static_cast<int>(discoverySocket->pendingDatagramSize()));
            QHostAddress from;
            discoverySocket->readDatagram(dg.data(), dg.size(), &from);
            const DiscoveredCam cam = parseProbeMatch(dg);
            if (cam.ip.isEmpty() || !cam.onvif) continue;   // ONVIF 아닌 WSD 장비는 무시

            // 같은 카메라가 응답을 여러 번(모델 있는 것 + scopes 빈 것) 보낸다 →
            // IP 기준 한 행만 유지. 이미 있으면 "더 나은 모델명"이 왔을 때만 갱신.
            const bool realModel = (cam.model != QStringLiteral("ONVIF 카메라"));
            if (discoverySeen.contains(cam.ip)) {
                if (realModel) {
                    for (int r = 0; r < discoveryTable->rowCount(); ++r) {
                        auto* ipItem = discoveryTable->item(r, 1);
                        auto* mdItem = discoveryTable->item(r, 0);
                        if (ipItem && ipItem->text() == cam.ip && mdItem &&
                            mdItem->text() == QStringLiteral("ONVIF 카메라"))
                            mdItem->setText(cam.model);  // 플레이스홀더 → 실제 모델
                    }
                }
                continue;
            }
            discoverySeen.insert(cam.ip);
            const int r = discoveryTable->rowCount();
            discoveryTable->insertRow(r);
            discoveryTable->setItem(r, 0, new QTableWidgetItem(cam.model));
            discoveryTable->setItem(r, 1, new QTableWidgetItem(cam.ip));
            discoveryTable->setItem(r, 2, new QTableWidgetItem(QStringLiteral("…")));
            syncDiscoveryTableHeight();   // 행이 늘어난 만큼만 표를 키운다

            // MAC 조회는 비동기로 — 여기서 arp를 동기(1.5초 블로킹)로 돌리면 그 사이
            // 몰려오는 다른 카메라 응답이 유실된다(첫 검색에서 1대만 잡히던 원인).
            const QString ip = cam.ip;
            auto* arp = new QProcess(this);
            connect(arp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [this, arp, ip](int, QProcess::ExitStatus) {
                        const QString out =
                            QString::fromLocal8Bit(arp->readAllStandardOutput());
                        arp->deleteLater();
                        static const QRegularExpression re(
                            QStringLiteral("([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"));
                        const auto m = re.match(out);
                        const QString mac = m.hasMatch()
                            ? m.captured(0).toUpper().replace('-', ':') : QStringLiteral("-");
                        for (int rr = 0; rr < discoveryTable->rowCount(); ++rr) {
                            auto* ipIt = discoveryTable->item(rr, 1);
                            auto* mcIt = discoveryTable->item(rr, 2);
                            if (ipIt && ipIt->text() == ip && mcIt) mcIt->setText(mac);
                        }
                    });
            arp->start(QStringLiteral("arp"), {QStringLiteral("-a"), ip});
        }
    });

    return camTab;
}

// 검색 결과 표를 "내용만큼만" 차지하게 한다 — 0행이면 숨기고, 있으면 행 수만큼
// (최대 6행, 넘으면 스크롤) 높이를 맞춘다. 빈 표를 늘 띄워두면 '연결' 카드 아래에
// 쓸데없는 빈 공간이 생긴다.
void MainWindow::syncDiscoveryTableHeight()
{
    if (!discoveryTable) return;
    const int rows = discoveryTable->rowCount();
    discoveryTable->setVisible(rows > 0);
    if (rows == 0) return;

    const int shown = qMin(rows, 6);
    int h = discoveryTable->horizontalHeader()->height() + 2 * discoveryTable->frameWidth();
    for (int r = 0; r < shown; ++r) h += discoveryTable->rowHeight(r);
    discoveryTable->setFixedHeight(h);
}

// 좌측 'ROI' 페이지 — 상태 + 단계 안내 + 큼직한 액션. 편집 영상은 우측 스테이지.
QWidget* MainWindow::buildCamRoiPage()
{
    auto* page = new QWidget();
    auto* roiV = new QVBoxLayout(page);
    roiV->setContentsMargins(0, 0, 0, 0);
    roiV->setSpacing(14);

    // 단계 안내 카드 — 남는 세로 공간을 채우려 늘어나지 않도록 Maximum 고정.
    auto* steps = new QFrame();
    steps->setObjectName("roiSteps");
    steps->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* sv = new QVBoxLayout(steps);
    sv->setContentsMargins(14, 12, 14, 12);
    sv->setSpacing(11);
    auto addStep = [&](const QString& n, const QString& text) {
        auto* row = new QHBoxLayout();
        row->setSpacing(10);
        auto* num = new QLabel(n);
        num->setObjectName("roiStepNum");
        num->setAlignment(Qt::AlignCenter);
        num->setFixedSize(22, 22);
        auto* t = new QLabel(text);
        t->setObjectName("roiStepText");
        t->setWordWrap(true);
        row->addWidget(num, 0, Qt::AlignVCenter);
        row->addWidget(t, 1, Qt::AlignVCenter);
        sv->addLayout(row);
    };
    addStep(QStringLiteral("1"), QStringLiteral("아래 ‘침대 추가’를 누릅니다."));
    addStep(QStringLiteral("2"), QStringLiteral("오른쪽 영상 위를 클릭해 침대 모서리를 찍습니다."));
    addStep(QStringLiteral("3"), QStringLiteral("더블클릭(또는 우클릭)으로 완료합니다."));
    addStep(QStringLiteral("4"), QStringLiteral("아래 목록에서 그 침대의 입소자를 지정합니다."));
    roiV->addWidget(steps);

    // 주 액션 — 침대 추가(그리는 중이면 '취소'로 토글). 전체 폭 강조 버튼.
    roiButton = new QPushButton(QStringLiteral("침대 추가"));
    roiButton->setObjectName("roiPrimary");
    roiButton->setCursor(Qt::PointingHandCursor);
    roiButton->setMinimumHeight(38);
    connect(roiButton, &QPushButton::clicked, this, &MainWindow::onRoiButtonClicked);
    roiV->addWidget(roiButton);

    // 보조 액션 — 제거 / 표시 토글
    roiClearButton = new QPushButton(QStringLiteral("침대 제거"));
    roiClearButton->setObjectName("roiClear");
    roiClearButton->setCursor(Qt::PointingHandCursor);
    connect(roiClearButton, &QPushButton::clicked, this, &MainWindow::onRoiClearClicked);
    roiToggleButton = new QPushButton(QStringLiteral("영상에 표시"));
    roiToggleButton->setObjectName("roiToggle");
    roiToggleButton->setCheckable(true);
    roiToggleButton->setChecked(true);
    roiToggleButton->setCursor(Qt::PointingHandCursor);
    connect(roiToggleButton, &QPushButton::toggled, this, &MainWindow::onRoiVisibilityToggled);
    auto* roiBtnRow = new QHBoxLayout();
    roiBtnRow->setSpacing(8);
    roiBtnRow->addWidget(roiClearButton, 1);
    roiBtnRow->addWidget(roiToggleButton, 1);
    roiV->addLayout(roiBtnRow);

    // ── 침대 목록 — 침대마다 "번호 · 입소자 콤보 · 삭제" 한 줄 ──
    // 이 목록이 곧 "어느 침대가 누구 자리인가" 매핑이다. 서버는 이 매핑을 앵커로
    // 추적 객체에 사람을 붙여 낙상·이탈 알림에 이름을 실어 보낸다.
    auto* bedsTitle = new QLabel(QStringLiteral("침대 · 입소자 매핑"));
    bedsTitle->setObjectName("roiStepText");
    roiV->addWidget(bedsTitle);

    auto* bedsBox = new QFrame();
    bedsBox->setObjectName("roiSteps");
    bedListLayout_ = new QVBoxLayout(bedsBox);
    bedListLayout_->setContentsMargins(12, 10, 12, 10);
    bedListLayout_->setSpacing(8);
    bedListEmpty_ = new QLabel(
        QStringLiteral("아직 지정된 침대가 없습니다.\n‘침대 추가’로 침대마다 영역을 그려 주세요."));
    bedListEmpty_->setObjectName("roiStepText");
    bedListEmpty_->setWordWrap(true);
    bedListLayout_->addWidget(bedListEmpty_);
    roiV->addWidget(bedsBox);

    roiV->addStretch();   // 남는 세로 공간은 아래로 — 단계/버튼이 벌어지지 않게
    return page;
}

// 인스펙터의 침대 목록을 현재 채널 기준으로 다시 만든다.
// 침대가 늘거나 줄면 줄 개수 자체가 달라져서 글자만 갈아끼울 수 없다
// (바이탈 카드 rebuildVitalCards와 같은 이유).
void MainWindow::rebuildBedList()
{
    if (!bedListLayout_) return;

    // 기존 줄 제거 — 안내 라벨(bedListEmpty_)은 살려 두고 재사용한다.
    while (bedListLayout_->count() > 0) {
        QLayoutItem* item = bedListLayout_->takeAt(0);
        if (item->widget() && item->widget() != bedListEmpty_) item->widget()->deleteLater();
        delete item;
    }

    const int ch = roiEditChannel;
    // 빈 방에는 침대가 없다 — 목록까지 101호 것을 보여주면 그 방에 침대가 있는
    // 것으로 읽힌다. 데이터(roiZones_)는 그대로 두고 표시만 비운다.
    auto zones = (selectedRoom_ == 0) ? roiZones_[ch] : QVector<RoiZone>{};
    std::sort(zones.begin(), zones.end(),
              [](const RoiZone& a, const RoiZone& b) { return a.id < b.id; });

    if (zones.isEmpty()) {
        bedListEmpty_->setVisible(true);
        bedListLayout_->addWidget(bedListEmpty_);
        return;
    }
    bedListEmpty_->setVisible(false);

    const int selected = roiEditorView ? roiEditorView->selectedZone() : -1;

    for (const RoiZone& z : zones) {
        auto* row = new QWidget();
        row->setObjectName("roiBedRow");
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);

        // 침대 번호 배지 — 영상 오버레이와 같은 색을 써서 눈으로 짝지어진다.
        auto* dot = new QLabel(QString::number(z.id + 1));
        dot->setAlignment(Qt::AlignCenter);
        dot->setFixedSize(22, 22);
        const QColor c = VideoView::zoneColor(z.id);
        // 의도적 잔류(clickslider.cpp와 같은 이유): 배경이 VideoView::zoneColor()의
        // 8색 배열이라 영상 오버레이와 출처를 공유해야 하므로 QSS로 옮기지 않는다.
        // 전경색만 흰색→본문색 토큰(대비 개선)으로 바꿨다.
        dot->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:11px;"
                                          "font-weight:700; font-size:11px;")
                               .arg(c.name(), QString::fromLatin1(kTextMain)));
        h->addWidget(dot);

        // 입소자 선택 — 이 채널에 배정된 재원 입소자만 후보로 올린다.
        auto* combo = new QComboBox();
        combo->addItem(QStringLiteral("미지정"), 0);
        for (int rid : residentsByChannel_[ch]) {
            auto it = residentInfo_.constFind(rid);
            const QString name = (it != residentInfo_.constEnd() && !it->name.isEmpty())
                                     ? it->name
                                     : QStringLiteral("입소자 %1").arg(rid);
            combo->addItem(name, rid);
        }
        const int bound = roiResident_[ch].value(z.id, 0);
        int idx = combo->findData(bound);
        if (idx < 0 && bound > 0) {
            // 채널 배정이 바뀌어 후보에 없는 사람이 매핑돼 있는 경우 —
            // 조용히 "미지정"으로 떨구면 매핑이 사라진 걸 아무도 모른다.
            combo->addItem(QStringLiteral("입소자 %1 (채널 밖)").arg(bound), bound);
            idx = combo->count() - 1;
        }
        combo->setCurrentIndex(qMax(0, idx));
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, ch, id = z.id, combo](int i) {
                    const int rid = combo->itemData(i).toInt();
                    if (roiResident_[ch].value(id, 0) == rid) return;
                    roiResident_[ch][id] = rid;
                    sendRoiBind(ch, id, rid);
                    refreshRoiZones(ch);
                });
        h->addWidget(combo, 1);

        // 이 침대만 삭제
        auto* del = new QPushButton(QStringLiteral("삭제"));
        del->setObjectName("roiClear");
        del->setCursor(Qt::PointingHandCursor);
        connect(del, &QPushButton::clicked, this, [this, ch, id = z.id]() {
            if (roiEditorView) roiEditorView->setSelectedZone(id);
            onRoiClearClicked();
        });
        h->addWidget(del);

        // 영상에서 고른 침대를 목록에서도 알아볼 수 있게 강조 — 팔레트 토큰(selected
        // 속성)을 써서 라이트 테마에서도 보인다. 목록 재빌드마다 새로 만들어지므로
        // 목록 행 아바타와 같은 이유로 repolish 없이 생성 직후 속성 설정만으로 충분하다.
        row->setProperty("selected", z.id == selected);
        bedListLayout_->addWidget(row);
    }
}

// 우측 스테이지 — 라이브/ROI 편집 영상(0) + 이미지 Before/After 프리뷰(1)를 스택으로.
// 이미지 탭 실시간(적용 후) 프리뷰에 프레임을 넣는다. 상자 비율도 함께 맞춘다 —
// 세 곳(채널 전환·모드 전환·프레임 수신)에서 각자 setFrame만 부르면 상자는 계속
// 16:9 가정으로 남아, 카메라 해상도가 다를 때 다시 검은 여백이 생긴다.
void MainWindow::setImagePreviewFrame(const QPixmap& pm)
{
    if (imgWipe_) imgWipe_->setAfter(pm);
}

QWidget* MainWindow::buildCamStagePanel()
{
    camStageStack = new QStackedWidget();

    // 0) 라이브/ROI 편집 영상 (한 위젯을 채널 전환하며 재사용)
    roiEditorView = new VideoView(roiEditChannel);
    roiEditorView->setObjectName("video");
    roiEditorView->setMinimumHeight(400);
    roiEditorView->setCornerRadius(13);   // 스테이지 카드(14px)와 맞춘 둥근 모서리
    connect(roiEditorView, &VideoView::roiCompleted, this, &MainWindow::onRoiCompleted);
    connect(roiEditorView, &VideoView::drawModeChanged, this, [this](int, bool on) {
        roiDrawing = on;
        if (roiButton) {
            roiButton->setText(on ? QStringLiteral("그리기 취소")
                                  : QStringLiteral("침대 추가"));
            roiButton->setProperty("drawing", on);
            roiButton->style()->unpolish(roiButton);
            roiButton->style()->polish(roiButton);
        }
    });
    // 영상 위에서 침대를 클릭하면 인스펙터 목록의 강조도 따라간다
    connect(roiEditorView, &VideoView::zoneSelected, this, &MainWindow::onRoiZoneSelected);
    camStageStack->addWidget(roiEditorView);

    // 1) 적용 전/후 와이프 비교 — 영상 한 장이 스테이지를 그대로 채운다.
    //    연결·ROI 탭의 큰 영상과 같은 위젯 크기·같은 cover 규칙이라 틀이 어긋나지 않는다.
    imgWipe_ = new WipeCompare();
    imgWipe_->setObjectName("video");
    connect(imgWipe_, &WipeCompare::focusRequested, this,
            [this](float nx, float ny) { sendFocus(roiEditChannel, true, nx, ny); });
    camStageStack->addWidget(imgWipe_);

    // 실시간(After) 갱신은 폴링하지 않는다 — onStreamFrame이 프레임이 도착할 때마다
    // 라이브/ROI 영상과 같은 타이밍으로 직접 넣어준다(200ms 타이머 폴링 시절의
    // 최대 200ms 추가 지연과 매 틱 원본 리샘플링 부하를 제거).

    // 스택을 카드(#camStage)로 감싼다 — 좌측 컨트롤 카드와 같은 테두리·radius로
    // 짝을 맞춰야 영상이 페이지에 '얹힌' 게 아니라 '들어앉은' 것처럼 보인다.
    auto* stageCard = new QFrame();
    stageCard->setObjectName("camStage");
    auto* sl = new QVBoxLayout(stageCard);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(camStageStack);
    return stageCard;
}

// 공용 채널 전환 — 레일 강조 + 편집 영상/ROI 로드 + 상태 배지 갱신.
void MainWindow::selectCamChannel(int ch)
{
    if (ch < 0 || ch >= 4) return;
    for (int i = 0; i < 4; ++i)
        if (camChannelBtns[i]) camChannelBtns[i]->setChecked(i == ch);
    selectRoiChannel(ch);          // roiEditChannel 설정 + roiEditorView 로드

    // 이미지 프리뷰: 채널이 바뀌면 '적용 전'은 검은 화면으로 리셋(아직 이 채널에
    // 적용한 적 없으니), '적용 후(실시간)'는 새 채널 프레임으로 즉시 교체.
    if (imgWipe_) imgWipe_->clearBefore();   // 채널이 바뀌면 비교 대상도 새로 잡는다
    setImagePreviewFrame(lastFramePix_[ch]);

    loadImageParams(ch);         // 이 채널에 마지막으로 보낸 값으로 슬라이더를 맞춘다
    refreshCamChannelStatus();   // 배지·헤더·컨트롤 활성화까지 여기서 이어진다
}

// 연결/ROI/이미지 모드 전환 — 좌측 컨트롤 스택 + 우측 스테이지 스택 동기화.
void MainWindow::setCamMode(const QString& mode)
{
    camMode_ = mode;
    const QString modes[3] = {QStringLiteral("연결"), QStringLiteral("ROI"),
                              QStringLiteral("이미지")};
    int idx = 0;
    for (int i = 0; i < 3; ++i) {
        if (camModeBtns[i]) camModeBtns[i]->setChecked(modes[i] == mode);
        if (modes[i] == mode) idx = i;
    }
    if (camControlStack) camControlStack->setCurrentIndex(idx);
    // 이미지 모드만 Before/After 프리뷰, 나머지는 라이브/ROI 영상.
    const bool imageMode = (mode == QStringLiteral("이미지"));
    if (camStageStack)
        camStageStack->setCurrentIndex(imageMode ? 1 : 0);
    // 전환 직후 빈 화면이 깜빡이지 않게 최신 프레임으로 한 번 채운다
    // (이후는 onStreamFrame이 프레임마다 갱신).
    if (imageMode) setImagePreviewFrame(lastFramePix_[roiEditChannel]);
}

// 채널별 연결·ROI 지정 여부를 레일 배지에 반영.
void MainWindow::refreshCamChannelStatus()
{
    // 빈 방을 보는 중이면 채널 배지도 전부 미연결이어야 한다 — 여기서 cameraActive_만
    // 보면 102호를 골라 놓고 "● 연결"이 뜬다.
    const bool liveRoom = (selectedRoom_ == 0);
    for (int ch = 0; ch < 4; ++ch) {
        if (!camChannelStatus[ch]) continue;
        const bool connected = liveRoom && cameraActive_[ch];
        const int beds = roiZones_[ch].size();
        QString txt = connected ? QStringLiteral("● 연결") : QStringLiteral("○ 미연결");
        // 침대가 여러 개일 수 있으니 "ROI 있음"이 아니라 몇 개인지를 보여준다
        if (beds > 0) txt += QStringLiteral(" · 침대 %1").arg(beds);
        camChannelStatus[ch]->setText(txt);
        // ●/○ 접두는 ALERT-02의 색 외 채널이라 지우지 않는다 — connected 속성과
        // 같은 갱신 경로에서 함께 설정해 색과 모양이 어긋나지 않게 한다.
        camChannelStatus[ch]->setProperty("connected", connected);
        camChannelStatus[ch]->style()->unpolish(camChannelStatus[ch]);
        camChannelStatus[ch]->style()->polish(camChannelStatus[ch]);
        camChannelStatus[ch]->update();
    }

    // 인스펙터 헤더 — 지금 만지는 채널의 번호·연결 상태·주소.
    const int cur = roiEditChannel;
    if (camInspCh) camInspCh->setText(QStringLiteral("CH %1").arg(cur + 1));
    if (camInspPill) {
        const bool on = liveRoom && cameraActive_[cur];
        // 채널 상태 텍스트와 같은 어휘(●/○)를 붙여 두 배지의 표기를 통일한다.
        camInspPill->setText(on ? QStringLiteral("● 연결됨") : QStringLiteral("○ 미연결"));
        camInspPill->setProperty("connected", on);
        camInspPill->style()->unpolish(camInspPill);
        camInspPill->style()->polish(camInspPill);
        camInspPill->update();
    }
    if (camInspIp) {
        // URL에는 계정·비밀번호가 들어 있으므로 호스트만 보여준다.
        const QString host = liveRoom ? QUrl(lastCameraUrl_[cur]).host() : QString();
        camInspIp->setText(host.isEmpty()
                               ? QStringLiteral("카메라가 연결되지 않았습니다")
                               : QStringLiteral("%1 · RTSP profile2").arg(host));
    }

    refreshCamControlsEnabled();
}

// ROI 편집 채널 전환 — 그 채널 영상/기존 ROI를 편집기에 로드하고 버튼을 강조.
void MainWindow::selectRoiChannel(int ch)
{
    if (ch < 0 || ch >= 4) return;
    roiEditChannel = ch;
    for (int i = 0; i < 4; ++i)
        if (roiChannelButtons[i]) roiChannelButtons[i]->setChecked(i == ch);
    if (!roiEditorView) return;

    if (roiEditorView->drawMode()) roiEditorView->cancelDraft();
    roiEditorView->setChannel(ch);
    if (channelViews[ch]) {
        roiEditorView->setCameraConnected(channelViews[ch]->cameraConnected());
        roiEditorView->setLive(channelViews[ch]->live());  // LIVE 표시등도 그 채널 상태로
    }
    roiEditorView->setRoiVisible(!roiToggleButton || roiToggleButton->isChecked());
    refreshRoiZones(ch);   // 이 채널의 침대들을 편집기·목록에 로드
    // 다음 프레임부터 onStreamFrame이 이 편집기에 실시간 영상을 계속 넣어준다.
}

// 카메라 설정 탭이 현재 보이는 탭인지 — ROI/이미지 실시간 프리뷰는 이때만 갱신한다.
bool MainWindow::cameraSettingsVisible() const
{
    // 장치 설정 페이지가 열려 있고, 그 안의 서브탭이 '카메라' 일 때만 카메라 프리뷰를 갱신한다.
    // (알림 서브탭이 앞에 있으면 ROI/이미지 실시간 갱신은 의미가 없다.)
    return contentStack && deviceSettingsTab_ && deviceStack_ && cameraSettingsTab_ &&
           contentStack->currentWidget() == deviceSettingsTab_ &&
           deviceStack_->currentWidget() == cameraSettingsTab_;
}

// "연결" — 카메라 탭의 IP/계정/비번으로 바로 연결. 포트·프로파일은 고정값.
void MainWindow::onAddCameraClicked()
{
    if (!camIpEdit) return;   // 팝업 미구성(정상 흐름에선 발생 안 함)

    const QString ip = camIpEdit->text().trimmed();
    if (ip.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("CCTV IP를 입력하거나 검색 목록에서 선택하세요."));
        return;
    }
    // 포트(554)·프로파일(profile2)은 카메라 규약상 고정.
    connectCameraWith(ip, camUserEdit->text().trimmed(), camPwEdit->text(),
                      554, QStringLiteral("profile2"));
}

// 수동 입력/검색 두 경로가 공유하는 실제 연결 처리.
void MainWindow::connectCameraWith(const QString& ip, const QString& user,
                                   const QString& password, int port,
                                   const QString& profile)
{
    const QString prof = profile.trimmed().isEmpty()
                             ? QStringLiteral("profile2") : profile.trimmed();

    // 입력값 저장(폼 복원용). 비밀번호는 평문 저장을 피해 담지 않는다 — 다음
    // 연결 때 다시 입력한다(서버로는 v1 평문 TCP로 나가며, 추후 TLS 적용 예정).
    QSettings s;
    s.setValue(QStringLiteral("camera/ip"), ip);
    s.setValue(QStringLiteral("camera/user"), user);
    s.setValue(QStringLiteral("camera/port"), port);
    s.setValue(QStringLiteral("camera/profile"), prof);

    // IP 하나 → 4채널 URL 생성 → 채널별 담당 Pi로 전송(StreamClient가 라우팅).
    int sent = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const QString url = buildRtspUrl(ip, user, password, port, prof, ch);
        lastCameraUrl_[ch] = url;   // 재접속 시 자동 재전송용(세션 한정)
        cameraActive_[ch] = true;   // 연결됨(재시작 후에도 해제 가능하도록 지속 저장)
        videoCleared_[ch] = false;  // 다시 표시 대상
        if (sendCamera(ch, url)) ++sent;
    }
    // 표시 여부는 applyRoomView가 결정한다 — 빈 방을 보는 중에 연결했다면
    // 그 방 화면에 101호 영상이 튀어나오면 안 된다.
    applyRoomView();
    persistCameraActive();
    refreshCamChannelStatus();   // 채널 레일 배지에 연결 상태 반영

    if (sent == 0) {
        QMessageBox::warning(
            this, QStringLiteral("전송 실패"),
            QStringLiteral("영상 서버에 연결되어 있지 않습니다.\n"
                           "서버 연결 상태를 확인하세요."));
    } else if (sent < 4) {
        QMessageBox::information(
            this, QStringLiteral("일부 전송"),
            QStringLiteral("4채널 중 %1채널만 전송했습니다.\n"
                           "일부 Pi 서버가 끊겨 있습니다.").arg(sent));
    } else {
        QMessageBox::information(
            this, QStringLiteral("카메라 연결 요청"),
            QStringLiteral("4채널 연결 요청을 서버로 보냈습니다.\n"
                           "서버가 카메라를 여는 동안 잠시 후 영상이 표시됩니다."));
    }
}

// "검색" — 팝업 안(카메라 탭)의 discoveryTable에 인라인으로 결과를 채운다.
// 별도 창을 띄우지 않는다. 응답 파싱·표 추가는 build 시 연결한 readyRead 람다가 처리.
void MainWindow::onSearchCameraClicked()
{
    if (!discoverySocket || !discoveryTable) return;

    // 망이 없는 상태로 앱을 켰다면 bind가 실패한 채 남는다 — 검색할 때 다시 시도.
    if (discoverySocket->state() != QAbstractSocket::BoundState &&
        !discoverySocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress)) {
        if (discoveryStatus)
            discoveryStatus->setText(QStringLiteral(
                "검색용 소켓을 열지 못했습니다 — 네트워크 연결과 방화벽을 확인하세요."));
        return;
    }

    discoveryTable->setRowCount(0);
    syncDiscoveryTableHeight();   // 결과 지웠으니 다시 숨김
    discoverySeen.clear();
    discoverySweepQueue.clear();
    if (discoveryStatus)
        discoveryStatus->setText(QStringLiteral("같은 망의 ONVIF 카메라를 검색 중…"));

    const QHostAddress mcast(QStringLiteral("239.255.255.250"));
    QUdpSocket* sock = discoverySocket;
    const QList<LocalNet> nets = usableIpv4Nets();

    // ── 1) 멀티캐스트 + 서브넷 브로드캐스트 ────────────────────────────
    // 기본 멀티캐스트 인터페이스가 가상 어댑터(Hyper-V·VirtualBox·Tailscale)로 잡히면
    // 카메라가 Probe를 못 받는다 → IPv4 인터페이스마다 각각 쏜다. 스위치가 멀티캐스트를
    // 걸러버리는 망(IGMP 스누핑, AP 클라이언트 격리)에서는 브로드캐스트가 유일한 경로다.
    //
    // Probe는 호출할 때마다 새로 만든다 — 같은 MessageID를 재전송하면 SOAP-over-UDP
    // 중복 제거를 하는 카메라가 2·3번째 시도를 통째로 무시해 재전송이 무의미해진다.
    auto sendProbes = [sock, mcast, nets]() {
        const QByteArray probe = buildWsDiscoveryProbe();
        int sentOn = 0;
        for (const LocalNet& n : nets) {
            if (n.iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
                sock->setMulticastInterface(n.iface);
                if (sock->writeDatagram(probe, mcast, 3702) > 0) ++sentOn;
            }
            if (!n.bcast.isNull() && n.prefix < 32)
                sock->writeDatagram(probe, n.bcast, 3702);   // 멀티캐스트가 막힌 망 대비
        }
        if (sentOn == 0) sock->writeDatagram(probe, mcast, 3702);  // 폴백
    };

    sendProbes();                                  // UDP 유실·타이밍 대비 여러 번 재전송
    QTimer::singleShot(700, this, sendProbes);
    QTimer::singleShot(1600, this, sendProbes);
    QTimer::singleShot(3000, this, sendProbes);

    // ── 2) 유니캐스트 스윕 ─────────────────────────────────────────────
    // 멀티캐스트 디스커버리가 꺼진 카메라, 멀티캐스트를 막은 스위치에서도 3702 유니캐스트
    // Probe에는 대부분 답한다. 로컬 /24(~/23)를 한 대씩 두드린다.
    auto queueNet = [this](quint32 network, quint32 hosts) {
        for (quint32 i = 1; i < hosts - 1; ++i) discoverySweepQueue.append(network + i);
    };
    QSet<quint32> queuedNets;
    for (const LocalNet& n : nets) {
        // /23보다 넓은 대역(예: Hyper-V의 /20)은 대상이 수천 개라 훑지 않는다.
        // /31·/32(Tailscale 같은 터널)는 "같은 망"에 다른 호스트가 없다.
        if (n.prefix < 23 || n.prefix > 30) continue;
        const quint32 mask = 0xFFFFFFFFu << (32 - n.prefix);
        const quint32 network = n.addr.toIPv4Address() & mask;
        if (queuedNets.contains(network)) continue;
        queuedNets.insert(network);
        queueNet(network, 1u << (32 - n.prefix));
    }
    // IP칸에 로컬 대역 밖 주소가 적혀 있으면 그 /24도 함께 훑는다 — 라우팅만 되면
    // 다른 서브넷 카메라도 찾을 수 있다(멀티캐스트는 라우터를 못 넘는다).
    if (camIpEdit) {
        const QHostAddress typed(camIpEdit->text().trimmed());
        if (!typed.isNull() && typed.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 network = typed.toIPv4Address() & 0xFFFFFF00u;
            if (!queuedNets.contains(network)) {
                queuedNets.insert(network);
                queueNet(network, 256);
            }
        }
    }

    // 한 번에 다 쏘면 미응답 주소의 ARP 대기 때문에 송신 버퍼가 막힌다(WSAEWOULDBLOCK)
    // → 15ms마다 24개씩 나눠 보낸다. /24 하나가 약 0.16초.
    if (!discoverySweepTimer) {
        discoverySweepTimer = new QTimer(this);
        discoverySweepTimer->setInterval(15);
        connect(discoverySweepTimer, &QTimer::timeout, this, [this]() {
            if (!discoverySocket) { discoverySweepTimer->stop(); return; }
            const QByteArray probe = buildWsDiscoveryProbe();
            for (int i = 0; i < 24 && !discoverySweepQueue.isEmpty(); ++i)
                discoverySocket->writeDatagram(
                    probe, QHostAddress(discoverySweepQueue.takeFirst()), 3702);
            if (discoverySweepQueue.isEmpty()) discoverySweepTimer->stop();
        });
    }
    if (!discoverySweepQueue.isEmpty()) discoverySweepTimer->start();

    QTimer::singleShot(7000, this, [this]() {
        if (!discoveryStatus || !discoveryTable) return;
        const int n = discoveryTable->rowCount();
        discoveryStatus->setText(
            n > 0 ? QStringLiteral("검색 완료 — %1대 발견 (행을 클릭하면 IP가 채워집니다)").arg(n)
                  : QStringLiteral("검색 완료 — 카메라를 못 찾았습니다. 같은 공유기/스위치에 "
                                   "물려 있는지, 카메라 ONVIF 검색이 켜져 있는지, PC 방화벽이 "
                                   "Carenet의 UDP 수신을 막고 있지 않은지 확인하세요. 다른 대역이면 "
                                   "위 IP칸에 그 대역의 주소를 하나 적고 다시 검색하세요."));
    });
}

void MainWindow::onCameraClearClicked()
{
    bool any = false;
    for (int ch = 0; ch < 4; ++ch)
        if (cameraActive_[ch]) any = true;   // Qt 재시작 후엔 URL이 비어도 이 플래그로 판단
    if (!any) {
        QMessageBox::information(this, QStringLiteral("카메라 해제"),
                                 QStringLiteral("연결된 카메라가 없습니다."));
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("카메라 해제"),
            QStringLiteral("모든 채널의 카메라 연결을 해제할까요?\n"
                           "서버가 RTSP 연결을 끊고 대기 상태로 돌아갑니다."))
        != QMessageBox::Yes) {
        return;
    }

    for (int ch = 0; ch < 4; ++ch) {
        sendCameraClear(ch);            // 서버에 해제 요청(연결 안 돼 있으면 무시됨)
        lastCameraUrl_[ch].clear();     // 자동 재전송 대상에서 제외
        cameraActive_[ch] = false;      // 미연결로 표시(지속 저장 반영)
        videoCleared_[ch] = true;       // 이후 들어오는 잔여 프레임 무시(검은 화면 유지)
        if (channelViews[ch]) {
            channelViews[ch]->setLive(false);
            channelViews[ch]->setCameraConnected(false);  // "카메라 미연결" 표시로 복귀
        }
        if (camThumbs[ch]) camThumbs[ch]->clearFrame();   // 스트립 썸네일도 비움
        lastFramePix_[ch] = QPixmap();   // 프리뷰가 옛 프레임을 다시 띄우지 않게
    }
    if (roiEditorView) {
        roiEditorView->setLive(false);
        roiEditorView->setCameraConnected(false);
    }
    if (imgWipe_) imgWipe_->clearFrames();
    applyRoomView();             // videoSuppressed_는 여기서만 계산된다
    persistCameraActive();
    refreshCamChannelStatus();   // 채널 타일 배지 + 인스펙터 헤더에 해제 상태 반영
}

// 활성 채널 집합을 QSettings에 비트마스크로 저장/복원한다. URL(비밀번호)은 담지 않는다.
void MainWindow::persistCameraActive()
{
    int mask = 0;
    for (int ch = 0; ch < 4; ++ch)
        if (cameraActive_[ch]) mask |= (1 << ch);
    QSettings s;
    s.setValue(QStringLiteral("camera/active_mask"), mask);
}

// 시작 시 호출 — 서버가 아직 스트리밍 중인 채널을 "연결됨"으로 복원해 해제를 가능케 한다.
// 프레임이 오면 setLive(true)로 실제 영상이 뜨고, 그전까지는 "신호 대기 중…"을 보여준다.
void MainWindow::restoreCameraActive()
{
    QSettings s;
    const int mask = s.value(QStringLiteral("camera/active_mask"), 0).toInt();
    for (int ch = 0; ch < 4; ++ch) {
        cameraActive_[ch] = (mask & (1 << ch)) != 0;
    }
    applyRoomView();
}

// Pi가 (재)연결되면, 그 Pi 담당 채널의 마지막 카메라 URL을 자동으로 다시 보낸다.
// 서버는 재부팅/재접속 후 카메라를 모르는 상태이므로, 사용자가 다시 누르지 않아도
// 세션 중 지정해 둔 카메라가 자동 복구된다.
void MainWindow::resendCamerasForServer(int serverIdx)
{
    for (int ch = 0; ch < 4; ++ch) {
        if (serverForChannel(ch) != serverIdx) continue;
        if (lastCameraUrl_[ch].isEmpty()) continue;
        if (sendCamera(ch, lastCameraUrl_[ch])) {
            videoCleared_[ch] = false;   // 표시 재개(연결 플래그는 이미 서 있다)
            qDebug() << "Pi" << serverIdx << "재접속 → ch" << ch << "카메라 자동 재전송";
        }
    }
    applyRoomView();   // 표시 여부는 지금 보고 있는 방이 결정한다
}
