// MainWindow — 생성·소멸, 화면 골격(네비·스택), 로그아웃, 스타일, 시계
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

MainWindow::MainWindow(const Auth::SessionUser& user, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , currentUser(user)
{
    ui->setupUi(this);

    // 채널별 환자 정보는 DB(residents)에서 채운다 — 하드코딩하지 않는다.
    // camera_id로 채널에 매핑되므로, 입소자를 등록해야 해당 채널에 이름이 뜬다.
    // (main.cpp에서 DB 연결을 이미 열어둬 buildUi 전에 조회 가능)
    loadPatientsFromDb();

    // ClickSlider 등 생성 시점에 theme.h 전역 색 상수를 인라인 QSS로 굽는
    // 위젯이 있으므로, buildUi()보다 먼저 팔레트를 확정해야 한다. 순서가
    // 뒤바뀌면 다크로 시작해도 그런 위젯만 라이트 색으로 남는다.
    applyPalette(darkMode ? kDark : kLight);  // 기본 다크 팔레트로 시작
    buildUi();
    applyTheme();
    if (themeToggleButton)
        themeToggleButton->setText(darkMode ? QStringLiteral("☀")
                                            : QStringLiteral("🌙"));
    enableDarkTitleBar(this);  // Windows 네이티브 타이틀바를 다크로

    // DB 입소자 목록(카드) 초기 로드 (main.cpp에서 연결을 이미 열어둠)
    refreshResidentCards();

    // 침대 ROI·입소자 매핑 복원 — buildUi 이후여야 오버레이/목록 위젯이 존재한다.
    // 서버도 부팅 때 같은 표를 읽으므로 양쪽이 같은 침대를 본다.
    loadRoiZonesFromDb();

    // 이전 세션에서 연결해 둔 카메라 채널을 복원한다. 서버는 Qt 재시작과 무관하게
    // 스트리밍을 유지하므로, URL이 없어도 활성 채널을 알면 "해제"가 정상 동작한다.
    restoreCameraActive();

    // 2. 영상 서버 통신 — 소켓·TLS·재접속·패킷 파싱은 StreamClient가 전담하고,
    //    이 창은 그 결과(프레임·이벤트·검색결과·연결상태)를 화면에 그리기만 한다.
    stream_ = new StreamClient(this);
    connect(stream_, &StreamClient::frameReceived, this, &MainWindow::onStreamFrame);
    connect(stream_, &StreamClient::connectionStateChanged, this, &MainWindow::onStreamStateChanged);
    connect(stream_, &StreamClient::serverConnected, this, &MainWindow::resendCamerasForServer);
    connect(stream_, &StreamClient::searchResultReceived, this, &MainWindow::onSearchResultReceived);
    // 침대 번호 유효 범위(kMaxRoiZones) 판정은 침대 목록을 아는 이쪽에서 한다.
    auto roiOf = [](int r) { return (r >= 0 && r < kMaxRoiZones) ? r : -1; };
    connect(stream_, &StreamClient::fallEvent, this,
            [this, roiOf](int ch, int r, quint64 ts, float nx, float ny) {
                handleFallEvent(ch, roiOf(r), ts, nx, ny);
            });
    connect(stream_, &StreamClient::bedEgressEvent, this,
            [this, roiOf](int ch, int r, quint64 ts, float nx, float ny) {
                handleBedEgressEvent(ch, roiOf(r), ts, nx, ny);
            });
    connect(stream_, &StreamClient::vitalAbnormalEvent, this,
            [this](int ch, quint64 ts) { handleVitalAbnormalEvent(ch, ts); });
    for (int ch = 0; ch < 4; ++ch)   // applyRoomView()가 미리 계산해 둔 차단 상태 반영
        stream_->setChannelSuppressed(ch, videoSuppressed_[ch]);
    // 3. 명세서 스펙: 5500번 포트로 즉시 접속 (끊기면 StreamClient가 자동 재접속)
    stream_->start();

    // 4. 상단 시계 / 웨어러블 바이탈 타이머 가동
    connect(&clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    clockTimer.start(1000);
    updateClock();

    connect(&vitalsTimer, &QTimer::timeout, this, &MainWindow::updateVitals);
    vitalsTimer.start(2000);
    updateVitals();

    // 메인 화면 채널 상태 배지(LIVE/미연결)가 소켓 상태만으로는 못 잡는 "카메라
    // 쪽만 멎은" 경우까지 반영하도록 주기 점검한다.
    connect(&channelHealthTimer, &QTimer::timeout, this, &MainWindow::checkChannelHealth);
    channelHealthTimer.start(kChannelHealthCheckMs);

    // MQTT 브로커 접속 — 웨어러블 생체·낙상을 받고, 알림 노드에 제어 명령을 보낸다.
    // 영상 경로(TCP 5500)와는 완전히 별개의 연결이다.
    // 브로커가 아직 안 떠 있어도 MqttQtManager 가 5초마다 다시 붙으려 시도하므로
    // 여기서 실패를 따로 처리하지 않는다.
    mqtt = new MqttQtManager(this);
    connect(mqtt, &MqttQtManager::wearableDataReceived, this, &MainWindow::onWearableData);
    connect(mqtt, &MqttQtManager::alarmCommandReceived, this, &MainWindow::onMqttAlarm);
    connect(mqtt, &MqttQtManager::connected,            this, &MainWindow::onMqttConnected);
    connect(mqtt, &MqttQtManager::disconnected,         this, &MainWindow::onMqttDisconnected);
    connect(mqtt, &MqttQtManager::connectionError,      this, &MainWindow::onMqttError);
    connect(mqtt, &MqttQtManager::nodeOnlineChanged,    this, &MainWindow::onAlarmNodeStatus);
    connect(mqtt, &MqttQtManager::payloadRejected, this,
            [](const QString& topic, const QString& why) {
                // 다른 노드가 형식을 바꿨을 때 조용히 묻히지 않게 남긴다.
                qWarning() << "[MQTT] 형식이 맞지 않는 메시지 무시:" << topic << why;
            });
    // TLS(MQTTS) 설정은 init() 보다 먼저 해야 첫 접속부터 암호화된다.
    // ca.crt 가 없으면 경고만 남기고 평문으로 붙는다 — 인증서를 아직
    // 못 받은 개발 PC 에서도 앱은 뜨게 하려는 의도다.
    const QString caPath = brokerCaPath();
    int port = brokerPort();
    if (QFile::exists(caPath)) {
        mqtt->setTlsConfig(caPath);
    } else {
        // 평문으로 내려갈 때는 포트도 같이 내려야 한다. 기본값 8883 은 TLS 전용이라
        // 평문으로 말을 걸면 브로커가 핸드셰이크 없이 곧바로 끊고, 화면에는 원인을
        // 알 수 없는 "네트워크 연결이 끊겼습니다"만 반복해서 뜬다.
        // 사용자가 포트를 직접 지정해 둔 경우에는 그 뜻을 존중해 건드리지 않는다.
        if (!QSettings().contains(kSettingsBrokerPort))
            port = 1883;
        qWarning() << "[MQTT] CA 인증서가 없어 평문(" << port << ")으로 접속합니다:" << caPath;
    }
    mqtt->init(brokerHost(), port);

    // 케어 타임 대시보드: 10초마다 care_logs를 재조회해 채널별 케어시간 갱신.
    connect(&careTimeTimer, &QTimer::timeout, this, &MainWindow::updateCareTime);
    careTimeTimer.start(10000);
    updateCareTime();

    // [제거됨] 예전에는 여기서 각 Pi의 /list(클립 HTTP)를 받아 파일명으로 이벤트
    // 표를 채웠다. 이제 이벤트 기록은 DB 원장(events)이 유일한 출처다
    // (reloadEventLog). 파일명 방식은 원장보다 아는 게 적었다:
    //   · 입소자·출처(카메라/웨어러블)·확인 여부를 알 수 없다
    //   · ch3_123_VITAL_ABNORMAL.mp4 를 '_'로 잘라 parts[2]="VITAL"만 보므로
    //     생체신호 이상 클립이 전부 "낙상"으로 잘못 분류됐다
    //   · 클립 보관기간이 지나 파일이 지워지면 사건 자체가 목록에서 사라졌다
    // 게다가 이 블록은 buildUi() 뒤에 돌면서 표를 setRowCount(0)로 비워, 원장에서
    // 읽어 온 행까지 지웠다. 클립 재생 URL은 events.clip_url(파일명)에 채널별
    // 호스트를 붙여 만든다 — 같은 규칙이라 재생 동작은 그대로다.
    
    //-- 오디오 음성 송출 (방송) 
    // 1. AudioTransmitter 초기화
    m_transmitter = new AudioTransmitter(this);

}

MainWindow::~MainWindow()
{
    // 자식 QObject(소켓·QProcess·타이머 …)는 이 소멸자 본문이 끝난 뒤 ~QObject 단계에서
    // 파괴되면서 마지막 신호를 한 번 더 낸다 — QSslSocket이 내는
    // stateChanged(UnconnectedState)가 대표적이다. 그 시점엔 MainWindow 부분이 이미
    // 파괴돼 있어 Qt6의 assertObjectType이 "destructor may have already run"으로
    // qFatal(0xc0000602)을 걸고 프로세스를 죽인다. 로그아웃해도 로그인 창으로 못
    // 돌아가고 앱이 통째로 종료되던 원인이 이것이다(크래시 덤프로 확인).
    // 파괴가 시작되기 전에 이 창으로 들어오는 신호선을 모두 끊는다.
    if (stream_) {
        stream_->disconnect(this);   // 이 창으로 오는 신호 차단이 먼저다
        stream_->stop();             // 그 뒤에 끊어야 종료 신호가 슬롯을 타지 않는다
    }
    for (QObject* child : findChildren<QObject*>())
        child->disconnect(this);

    delete ui;
}


// 좌측 네비 레일 — 아이콘+라벨 6개, 접으면 아이콘만. 상태는 QSettings에 남는다.
QWidget* MainWindow::buildNavRail()
{
    navRail = new QFrame();
    navRail->setObjectName("navRail");
    auto* v = new QVBoxLayout(navRail);
    v->setContentsMargins(10, 12, 10, 12);
    v->setSpacing(4);

    navToggle = new QPushButton(QStringLiteral("☰"));
    navToggle->setObjectName("navToggle");
    navToggle->setCursor(Qt::PointingHandCursor);
    navToggle->setFixedSize(34, 32);
    connect(navToggle, &QPushButton::clicked, this,
            [this] { setNavCollapsed(!navCollapsed_); });
    auto* toggleRow = new QHBoxLayout();
    toggleRow->setContentsMargins(0, 0, 0, 0);
    toggleRow->addWidget(navToggle);
    toggleRow->addStretch();
    v->addLayout(toggleRow);
    v->addSpacing(10);

    const QString names[kNavCount] = {
        QStringLiteral("실시간 관제"), QStringLiteral("이벤트 기록"),
        QStringLiteral("영상 검색"),   QStringLiteral("일일 리포트"),
        QStringLiteral("입소자 관리"), QStringLiteral("장치 설정")};
    for (int i = 0; i < kNavCount; ++i) {
        navBtns[i] = new QPushButton(names[i]);
        navBtns[i]->setObjectName("navBtn");
        navBtns[i]->setCheckable(true);
        navBtns[i]->setChecked(i == 0);
        navBtns[i]->setAutoExclusive(true);
        navBtns[i]->setCursor(Qt::PointingHandCursor);
        navBtns[i]->setIconSize(QSize(20, 20));
        navBtns[i]->setMinimumHeight(40);
        navBtns[i]->setToolTip(names[i]);   // 접었을 때 라벨 대신 알려준다
        connect(navBtns[i], &QPushButton::clicked, this, [this, i] {
            if (contentStack) contentStack->setCurrentIndex(i);
        });
        v->addWidget(navBtns[i]);
    }
    v->addStretch();

    refreshNavIcons();
    QSettings s;
    setNavCollapsed(s.value(QStringLiteral("ui/nav_collapsed"), false).toBool());
    return navRail;
}

// 팔레트가 바뀌면 아이콘 색도 다시 입힌다. 체크 상태(On)는 흰색 — 악센트 배경 위라서.
void MainWindow::refreshNavIcons()
{
    for (int i = 0; i < kNavCount; ++i) {
        if (!navBtns[i]) continue;
        QIcon ic;
        ic.addPixmap(navIconPixmap(i, QColor(QString::fromLatin1(kTextSub))),
                     QIcon::Normal, QIcon::Off);
        ic.addPixmap(navIconPixmap(i, QColor(Qt::white)), QIcon::Normal, QIcon::On);
        navBtns[i]->setIcon(ic);
    }
}

void MainWindow::setNavCollapsed(bool on)
{
    navCollapsed_ = on;
    if (navRail) navRail->setFixedWidth(on ? 62 : 208);
    for (int i = 0; i < kNavCount; ++i) {
        if (!navBtns[i]) continue;
        // 접힘: 라벨을 지워 아이콘만 남긴다(툴팁이 이름을 대신한다).
        navBtns[i]->setText(on ? QString() : navBtns[i]->toolTip());
        navBtns[i]->setProperty("collapsed", on);
        navBtns[i]->style()->unpolish(navBtns[i]);
        navBtns[i]->style()->polish(navBtns[i]);
    }
    QSettings s;
    s.setValue(QStringLiteral("ui/nav_collapsed"), on);
}

void MainWindow::buildUi()
{
    auto* root = new QVBoxLayout(ui->centralwidget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    // 화면 전환은 상단 탭이 아니라 좌측 네비 레일이 맡는다.
    // 관제 그리드는 세로가 모자라고 가로가 남는 비율(16:9 4분할)이라, 탭바가
    // 먹던 높이를 레일의 폭으로 옮기면 영상이 오히려 커진다.
    contentStack = new QStackedWidget();
    contentStack->setObjectName("contentStack");

    // ── 1: 실시간 관제 및 제어 (경보 배너 + 영상월 + 바이탈 패널) ──
    // 좌→우 세 덩어리: 리소스 트리 · 영상월(레이아웃 탭+그리드+타임라인) · 바이탈.
    // Wisenet Viewer도 같은 3분할이며(우측은 이벤트 패널), 우리는 그 자리에
    // 웨어러블 바이탈을 둔다 — 요양원 관제에서 더 자주 보는 정보다.
    auto* body = new QHBoxLayout();
    body->setContentsMargins(12, 12, 12, 12);
    body->setSpacing(12);
    body->addWidget(buildResourcePanel(), 0);
    body->addWidget(buildVideoWall(), 1);
    body->addWidget(buildVitalsPanel(), 0);

    // [제거됨] 예전에는 여기 대시보드 최상단에 가로 전체를 먹는 48px 빨간 띠
    // (#alertBanner)를 상시로 띄웠다. 경보 알림 경로가 이미 셋이라 뺐다:
    //   · 위에서 내려오는 토스트(#alarmToast) — "무슨 일이 어디서"
    //   · 창 전체 빨강 펄스 테두리(AlarmOverlay) — 시야 밖에 있어도 눈에 띔
    //   · 해당 채널 타일 빨간 테두리 점멸(VideoView::setAlert) — "어느 화면"
    // 띠까지 더하면 정작 봐야 할 영상이 그만큼 밀려 내려간다.
    auto* dashboardOuter = new QVBoxLayout();
    dashboardOuter->setContentsMargins(0, 0, 0, 0);
    dashboardOuter->setSpacing(0);
    dashboardOuter->addLayout(body, 1);

    auto* dashboardTab = new QWidget();
    dashboardTab->setLayout(dashboardOuter);
    contentStack->addWidget(dashboardTab);

    contentStack->addWidget(buildEventLogTab());        // 2: 이벤트 기록
    contentStack->addWidget(buildVideoSearchTab());     // 3: 영상 검색
    contentStack->addWidget(buildReportPage());         // 4: 일일 리포트
    contentStack->addWidget(buildDbTab());              // 5: 입소자 관리
    contentStack->addWidget(buildDeviceSettingsTab());  // 6: 장치 설정(카메라 + 알림)

    auto* shell = new QHBoxLayout();
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    shell->addWidget(buildNavRail(), 0);
    shell->addWidget(contentStack, 1);
    root->addLayout(shell, 1);

    // 경보 펄스 오버레이 — 중앙 위젯 전체를 덮되 테두리만 그린다(마우스 통과).
    alarmOverlay_ = new AlarmOverlay(ui->centralwidget);
    alarmOverlay_->setGeometry(ui->centralwidget->rect());

    // 경보 토스트 — 상단에서 슬라이드해 내려오는 알림(오버레이, 레이아웃 밖).
    buildAlarmBanner();

    resize(1680, 960);
    // 리소스 패널(208) + 네비 레일(208) + 바이탈(316)이 동시에 펼쳐진 상태에서도
    // 영상이 2×2로 쓸 만하게 남는 폭. 둘 중 하나를 접으면 훨씬 여유로워진다.
    setMinimumSize(1420, 780);
}

// 창 크기가 바뀌면 경보 오버레이도 중앙 위젯 크기에 맞춘다.
void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    if (alarmOverlay_ && ui && ui->centralwidget)
        alarmOverlay_->setGeometry(ui->centralwidget->rect());
    // 경보 토스트가 떠 있으면 가로 중앙으로 다시 맞춘다.
    if (alarmToastShown_ && alarmBanner_ && ui && ui->centralwidget) {
        const int x = qMax(12, (ui->centralwidget->width() - alarmBanner_->width()) / 2);
        alarmBanner_->move(x, 78);
    }
}

QWidget* MainWindow::buildHeader()
{
    auto* header = new QFrame();
    header->setObjectName("header");
    header->setFixedHeight(68);

    auto* lay = new QHBoxLayout(header);
    lay->setContentsMargins(22, 0, 18, 0);
    lay->setSpacing(10);

    // 로고 / 타이틀 — 워드마크만 (브랜드 점·부제목 제거)
    auto* logo = new QLabel(QStringLiteral("Carenet"));
    logo->setObjectName("logo");

    lay->addWidget(logo);
    lay->addStretch();

    // ── 실시간 관제 액션 — 방송(인터콤) ──
    // 경보 해제는 헤더가 아니라 "경보 배너"(경보 시에만 표시)로 옮겼다.
    micButton = new QPushButton(QStringLiteral("🎤 방송"));
    micButton->setObjectName("micButton");
    micButton->setCursor(Qt::PointingHandCursor);
    // 누르고 있는 동안만 방송하던 방식(press-and-hold)에서,
    // 한 번 클릭하면 방송 시작 · 다시 클릭하면 종료되는 토글 방식으로 변경.
    micButton->setCheckable(true);
    connect(micButton, &QPushButton::toggled, this, &MainWindow::onMicToggled);
    lay->addWidget(micButton);

    // 연결 상태 — pill 배지
    auto* statusPill = new QFrame();
    statusPill->setObjectName("statusPill");
    auto* spLay = new QHBoxLayout(statusPill);
    spLay->setContentsMargins(11, 3, 13, 3);
    spLay->setSpacing(7);
    statusDot = new QLabel();
    statusDot->setObjectName("statusDot");
    statusDot->setFixedSize(7, 7);
    statusText = new QLabel();
    statusText->setObjectName("statusText");
    spLay->addWidget(statusDot);
    spLay->addWidget(statusText);
    lay->addWidget(statusPill);
    // "영상 서버 연결됨" 배지는 화면에서 감춘다 — statusDot/statusText 객체는 연결 상태
    // 갱신 로직(setConnectionState 등)이 참조하므로 그대로 살려두고 표시만 끈다.
    statusPill->hide();

    // 실시간 시계
    clockLabel = new QLabel();
    clockLabel->setObjectName("clock");
    lay->addWidget(clockLabel);

    // 도움말 — 원형 물음표 아이콘 + "도움말" 텍스트 (한화 웹UI 헤더와 유사).
    // 아이콘은 직접 그려 넣는다 — QPushButton은 아이콘+텍스트를 정상 배치/측정한다.
    helpButton = new QPushButton(QStringLiteral("도움말"));
    helpButton->setObjectName("helpBtn");
    helpButton->setCursor(Qt::PointingHandCursor);
    helpButton->setToolTip(QStringLiteral("Carenet 도움말 — 기능 설명"));
    {
        const int d = 20;
        const qreal dpr = 2.0;               // 고해상도로 그려 또렷하게
        QPixmap pm(int(d * dpr), int(d * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter pt(&pm);
        pt.setRenderHint(QPainter::Antialiasing, true);
        const QColor ic(0x8B, 0x98, 0xA5);   // 두 테마 모두에서 보이는 중간 회색
        QPen pen(ic);
        pen.setWidthF(1.4);
        pt.setPen(pen);
        // 원 — 여백을 조금만 두고 꽉 차게
        const qreal m = 1.2;
        pt.drawEllipse(QRectF(m, m, d - 2 * m, d - 2 * m));
        // 물음표 — 타이트 바운딩 박스로 원 정중앙에 딱 맞춘다
        QFont f = pt.font();
        f.setPixelSize(13);
        f.setBold(true);
        pt.setFont(f);
        const QString q = QStringLiteral("?");
        const QFontMetricsF fm(f);
        const QRectF br = fm.tightBoundingRect(q);
        const qreal cx = d / 2.0, cy = d / 2.0;
        pt.drawText(QPointF(cx - (br.x() + br.width() / 2.0),
                            cy - (br.y() + br.height() / 2.0)), q);
        pt.end();
        helpButton->setIcon(QIcon(pm));
        helpButton->setIconSize(QSize(d, d));
    }
    connect(helpButton, &QPushButton::clicked, this, &MainWindow::onHelpClicked);
    lay->addWidget(helpButton);

    // 테마 토글
    themeToggleButton = new QPushButton(QStringLiteral("🌙"));
    themeToggleButton->setObjectName("themeToggle");
    themeToggleButton->setCursor(Qt::PointingHandCursor);
    themeToggleButton->setToolTip(QStringLiteral("라이트/다크 테마 전환"));
    connect(themeToggleButton, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    lay->addWidget(themeToggleButton);

    // ── 계정 칩 — 아바타 + 이름 + 로그아웃을 하나의 캡슐로 묶는다 ──
    // 세로 구분선을 없애고, 헤더 우측을 "정보(상태·시계) / 계정" 두 덩어리로만 나눈다.
    auto* userChip = new QFrame();
    userChip->setObjectName("userChip");
    auto* ucl = new QHBoxLayout(userChip);
    ucl->setContentsMargins(4, 4, 6, 4);
    ucl->setSpacing(8);

    userNameLabel = new QLabel();
    userNameLabel->setObjectName("userName");
    userNameLabel->setText(currentUser.name);
    userNameLabel->setToolTip(QStringLiteral("%1 (%2)")
                                  .arg(currentUser.name, currentUser.loginId));

    logoutButton = new QPushButton(QStringLiteral("로그아웃"));
    logoutButton->setObjectName("logoutButton");
    logoutButton->setCursor(Qt::PointingHandCursor);
    connect(logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);

    ucl->addWidget(userNameLabel);
    ucl->addWidget(logoutButton);
    lay->addWidget(userChip);

    return header;
}

// 경보 토스트 — 감지 시 화면 상단에서 아래로 슬라이드해 내려오는 알림 카드(오버레이).
// "채널 N에서 낙상 발생!" + [경보 해제]. 평상시엔 화면 위로 파킹돼 안 보인다.
QWidget* MainWindow::buildAlarmBanner()
{
    alarmBanner_ = new QFrame(ui->centralwidget);   // 레이아웃 밖 오버레이
    alarmBanner_->setObjectName("alarmToast");
    applyCardShadow(alarmBanner_, 30, 10, 110);      // 떠 있는 느낌의 진한 그림자
    auto* lay = new QHBoxLayout(alarmBanner_);
    lay->setContentsMargins(18, 12, 14, 12);
    lay->setSpacing(12);

    auto* dot = new QLabel();
    dot->setObjectName("alarmDot");
    dot->setFixedSize(9, 9);
    lay->addWidget(dot);

    alarmSummaryLabel_ = new QLabel(QStringLiteral("낙상 발생!"));
    alarmSummaryLabel_->setObjectName("alarmToastText");
    lay->addWidget(alarmSummaryLabel_);
    lay->addSpacing(8);

    alarmClearButton = new QPushButton(QStringLiteral("경보 해제"));
    alarmClearButton->setObjectName("alarmToastBtn");
    alarmClearButton->setCursor(Qt::PointingHandCursor);
    connect(alarmClearButton, &QPushButton::clicked, this, &MainWindow::onAlarmClearClicked);
    lay->addWidget(alarmClearButton);

    alarmAnim_ = new QPropertyAnimation(alarmBanner_, "pos", this);
    alarmAnim_->setDuration(300);
    alarmAnim_->setEasingCurve(QEasingCurve::OutCubic);

    alarmBanner_->adjustSize();
    alarmBanner_->move(0, -alarmBanner_->height() - 24);   // 화면 위로 파킹
    return alarmBanner_;
}

// 활성 경보를 모아 토스트 문구를 만들고, 있으면 내려오게(없으면 올라가게) 한다.
void MainWindow::updateAlarmBanner()
{
    if (!alarmBanner_) return;
    // (채널, 종류) 수집
    QList<QPair<int, QString>> evts;
    for (int ch = 0; ch < 4; ++ch) {
        if (fallActive[ch])      evts.append({ch, QStringLiteral("낙상")});
        if (bedEgressActive[ch]) evts.append({ch, QStringLiteral("침상이탈")});
        if (vitalAbnormalActive[ch]) evts.append({ch, QStringLiteral("생체신호 이상")});
    }
    if (evts.isEmpty()) { animateAlarmToast(false); return; }

    QString msg;
    if (evts.size() == 1) {
        msg = QStringLiteral("채널 %1에서 %2 발생!")
                  .arg(evts[0].first + 1).arg(evts[0].second);
    } else {
        QStringList parts;
        for (const auto& e : evts)
            parts << QStringLiteral("채널 %1 %2").arg(e.first + 1).arg(e.second);
        msg = parts.join(QStringLiteral("   ·   ")) +
              QStringLiteral("   (%1건)").arg(evts.size());
    }
    if (alarmSummaryLabel_) alarmSummaryLabel_->setText(msg);
    animateAlarmToast(true);
}

// 토스트를 상단에서 아래로(show=true) 또는 위로(show=false) 슬라이드. 중앙 정렬.
void MainWindow::animateAlarmToast(bool show)
{
    if (!alarmBanner_ || !alarmAnim_ || !ui->centralwidget) return;
    alarmBanner_->adjustSize();
    const int w = alarmBanner_->width();
    const int x = qMax(12, (ui->centralwidget->width() - w) / 2);
    const int shownY = 78;                              // 헤더 바로 아래
    const int hiddenY = -alarmBanner_->height() - 24;   // 화면 위로

    if (show) {
        if (!alarmToastShown_) {
            alarmBanner_->move(x, hiddenY);
            alarmBanner_->show();
        }
        alarmBanner_->raise();
        alarmToastShown_ = true;
        alarmAnim_->stop();
        alarmAnim_->setStartValue(alarmBanner_->pos());
        alarmAnim_->setEndValue(QPoint(x, shownY));
        alarmAnim_->start();
    } else {
        if (!alarmToastShown_) return;
        alarmToastShown_ = false;
        alarmAnim_->stop();
        alarmAnim_->setStartValue(alarmBanner_->pos());
        alarmAnim_->setEndValue(QPoint(alarmBanner_->x(), hiddenY));
        alarmAnim_->start();
    }
}

// ═══════════════════════════════════════════════════════════
//  로그아웃 — 창을 닫고 main()의 루프가 로그인 창을 다시 띄운다
// ═══════════════════════════════════════════════════════════
void MainWindow::onLogoutClicked()
{
    // 관제 중 오조작으로 화면이 꺼지면 안 되므로 한 번 되묻는다.
    const auto answer = QMessageBox::question(
        this, QStringLiteral("로그아웃"),
        QStringLiteral("로그아웃하시겠습니까?\n관제 화면이 종료되고 로그인 화면으로 돌아갑니다."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    logoutRequested_ = true;
    // 도움말 등 다른 최상위 창이 떠 있으면 이 창만 닫아서는 "마지막 창"이 되지 않아
    // quitOnLastWindowClosed가 걸리지 않는다 → main()의 a.exec()가 돌아오지 못하고
    // 관제 화면만 사라진 채 로그인 창이 안 뜬다. 열린 창을 모두 정리하고 명시적으로
    // 이벤트 루프를 끝낸다.
    const auto tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops)
        if (w != this) w->close();
    close();
    qApp->quit();
}

QWidget* MainWindow::buildVideoWall()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(12, 10, 12, 10);
    outer->setSpacing(8);

    // 위: 레이아웃 프리셋 탭 (Wisenet Viewer 상단 레이아웃 탭에 대응)
    outer->addWidget(buildLayoutTabs());

    videoGrid = new QGridLayout();
    videoGrid->setSpacing(6);   // 관제용 밀집 배치 — 영상 면적을 최대한 남긴다
    for (int ch = 0; ch < 4; ++ch)
        videoCards[ch] = buildVideoCard(ch);
    // 시작 선택 채널의 테두리는 여기서 직접 켠다 — selectChannel()은 "이미 그
    // 채널"이라 조기 반환하므로 첫 한 번은 아무도 칠해 주지 않는다.
    if (videoCards[selectedChannel_])
        videoCards[selectedChannel_]->setProperty("selected", true);
    auto* gridHost = new QWidget();
    gridHost->setObjectName("gridHost");
    gridHost->setLayout(videoGrid);

    // 가운데: 라이브 그리드 <-> NVR 재생 화면.
    // 재생 중에도 소켓 수신은 계속 돌아가므로 라이브로 되돌아오면 곧장 최신
    // 프레임이 뜬다(재연결을 기다릴 필요가 없다).
    liveOrPlaybackStack_ = new QStackedWidget();
    liveOrPlaybackStack_->setObjectName("liveOrPlayback");
    liveOrPlaybackStack_->addWidget(gridHost);

    playbackVideo_ = new QVideoWidget();
    playbackVideo_->setObjectName("playbackVideo");
    liveOrPlaybackStack_->addWidget(playbackVideo_);

    playbackPlaceholder_ = new QLabel();
    playbackPlaceholder_->setObjectName("playbackHint");
    playbackPlaceholder_->setAlignment(Qt::AlignCenter);
    playbackPlaceholder_->setWordWrap(true);
    liveOrPlaybackStack_->addWidget(playbackPlaceholder_);
    outer->addWidget(liveOrPlaybackStack_, 1);

    // 아래: 재생 트랜스포트 + 타임라인
    outer->addWidget(buildTransportBar());

    relayoutGrid();
    return panel;
}

// 그리드 위 레이아웃 프리셋 탭 — Wisenet Viewer의 레이아웃 탭 자리.
// 관제사가 "4개 다 보기 / 하나 크게 + 나머지 작게 / 하나만" 사이를 오간다.
QWidget* MainWindow::buildLayoutTabs()
{
    auto* bar = new QFrame();
    bar->setObjectName("layoutTabBar");

    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    const QString names[3] = {QStringLiteral("2×2 전체"),
                              QStringLiteral("스포트라이트"),
                              QStringLiteral("단일 채널")};
    const GridLayout modes[3] = {GridLayout::Quad, GridLayout::Spotlight,
                                 GridLayout::Single};
    for (int i = 0; i < 3; ++i) {
        layoutTabBtns_[i] = new QPushButton(names[i]);
        layoutTabBtns_[i]->setObjectName("layoutTab");
        layoutTabBtns_[i]->setCheckable(true);
        layoutTabBtns_[i]->setAutoExclusive(true);
        layoutTabBtns_[i]->setChecked(i == 0);
        layoutTabBtns_[i]->setCursor(Qt::PointingHandCursor);
        connect(layoutTabBtns_[i], &QPushButton::clicked, this,
                [this, m = modes[i]] { setGridLayout(m); });
        lay->addWidget(layoutTabBtns_[i]);
    }

    lay->addStretch();

    // 숨긴 타일 되살리기 — x로 뺀 채널을 한 번에 되돌린다.
    auto* restoreBtn = new QPushButton(QStringLiteral("타일 모두 표시"));
    restoreBtn->setObjectName("layoutRestoreBtn");
    restoreBtn->setCursor(Qt::PointingHandCursor);
    connect(restoreBtn, &QPushButton::clicked, this, [this] {
        for (int ch = 0; ch < 4; ++ch) setTileHidden(ch, false);
    });
    lay->addWidget(restoreBtn);

    return bar;
}

QWidget* MainWindow::buildVideoCard(int channel)
{
    auto* card = new QFrame();
    // #videoTile — 관제 그리드 전용 이름. NVR 탐색·블랙박스 카드가 쓰는
    // #videoCard와 분리했다: 관제 타일만 각진 모서리 + 선택 테두리를 쓴다.
    card->setObjectName("videoTile");
    card->setProperty("selected", false);
    // 스포트라이트 시 작은 칸으로도 줄어들 수 있게 최소 크기를 낮게 잡는다.
    card->setMinimumSize(140, 96);
    // 마우스가 들어오고 나가는 걸 알아야 호버 툴바를 띄운다(eventFilter가 처리).
    card->installEventFilter(this);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(1, 1, 1, 1);   // 선택 테두리가 영상을 덮지 않게 1px
    lay->setSpacing(0);

    // 영상 영역 — VideoView가 프레임 + 오버레이(LIVE/이름) + ROI를 담당.
    auto* video = new VideoView(channel);
    video->setObjectName("video");
    video->setCornerRadius(0);   // Wisenet 톤 — 관제 타일은 각진 모서리
    channelViews[channel] = video;
    // 시작할 때부터 이름을 얹는다. refreshPatientLabels()는 입소자를 편집한 뒤에만
    // 불려서, 예전엔 첫 화면의 타일만 "CH1"로 남고 리소스 트리는 "CH1 · 김복순"으로
    // 떠 같은 채널이 두 이름으로 보였다(loadPatientsFromDb는 buildUi보다 먼저 돈다).
    video->setDisplayName(channelDisplayName(channel));
    // 침대 ROI는 장치 설정에서만 그린다 — 관제 타일은 표시만 한다.
    // (refreshRoiZones()가 여기에 영역을 밀어 넣어 오버레이로 보여준다)
    connect(video, &VideoView::tileClicked, this, &MainWindow::selectChannel);
    lay->addWidget(video, 1);

    buildTileChrome(channel, card);
    return card;
}

// 타일 위에 얹는 크롬 — 우상단 닫기(x) + 하단 가운데 호버 툴바.
// Wisenet Viewer의 타일과 같은 자리다. 툴바는 평소 숨어 있다가 마우스를 올리면
// 뜬다 — 4분할 화면에서 버튼이 늘 떠 있으면 영상보다 버튼이 먼저 눈에 들어온다.
// 타일 위에 얹는 크롬 — 우상단 닫기(x) + 하단 가운데 호버 툴바.
//
// 이 둘은 카드의 "직접 자식"이다. 예전엔 투명 오버레이(WA_TransparentForMouseEvents)
// 안에 레이아웃으로 넣었는데, 그 속성은 위젯뿐 아니라 그 자식들에게 가는 마우스
// 이벤트까지 끊는다 — 버튼이 보이고 호버도 되는데 눌리지는 않았다.
// 카드의 자식으로 두면 아래 VideoView와 형제가 되어, 버튼 위 클릭은 버튼이,
// 나머지 영역 클릭은 영상이 그대로 받는다(ROI 그리기가 막히지 않는다).
// 위치는 레이아웃이 아니라 layoutTileChrome()이 카드 크기에 맞춰 직접 잡는다.
QWidget* MainWindow::buildTileChrome(int channel, QWidget* card)
{
    // 우상단: 레이아웃에서 이 타일 빼기.
    // 카메라 연결을 끊는 게 아니라 "이 화면에서 안 본다"는 뜻이다(Wisenet과 동일).
    // 서버는 계속 녹화·분석하고, 리소스 트리에서 다시 누르면 돌아온다.
    auto* closeBtn = new QPushButton(QStringLiteral("✕"), card);
    closeBtn->setObjectName("tileClose");
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(20, 20);
    closeBtn->setToolTip(QStringLiteral("이 타일을 레이아웃에서 제거 (카메라는 계속 녹화)"));
    connect(closeBtn, &QPushButton::clicked, this,
            [this, channel] { setTileHidden(channel, true); });
    tileCloseBtns_[channel] = closeBtn;

    // 하단 가운데: 호버 툴바. 평소 숨어 있다가 마우스를 올리면 뜬다 — 4분할
    // 화면에서 버튼이 늘 떠 있으면 영상보다 버튼이 먼저 눈에 들어온다.
    auto* toolbar = new QFrame(card);
    toolbar->setObjectName("tileToolbar");
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(6, 4, 6, 4);
    tb->setSpacing(2);

    // 관제 화면 타일에는 "보는 동작"만 둔다. 침대 ROI를 그리고 표시하는 건 설정이라
    // 장치 설정 → 카메라 → ROI 탭에서 한다 — 관제하다 실수로 영역을 다시 그리면
    // 낙상·이탈 판정 기준이 조용히 바뀐다.
    const char16_t* tips[2] = {
        u"이 채널만 크게 보기 (다시 누르면 2×2로)",
        u"현재 화면 저장(스냅샷)",
    };
    for (int i = 0; i < 2; ++i) {
        auto* b = new QPushButton();
        b->setObjectName("tileToolBtn");
        // 영상(검정) 위에 늘 얹히는 버튼이라 아이콘 색은 팔레트가 아니라 고정 회백이다.
        b->setIcon(QIcon(tileToolIconPixmap(i, QColor(0xE6, 0xED, 0xF3))));
        b->setIconSize(QSize(15, 15));
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(26, 22);
        b->setToolTip(QString::fromUtf16(tips[i]));
        connect(b, &QPushButton::clicked, this, [this, channel, i] {
            if (selectedRoom_ != 0) return;   // 빈 타일은 확대할 것도 저장할 것도 없다
            selectChannel(channel);
            if (i == 0) {
                setGridLayout(gridLayout_ == GridLayout::Single ? GridLayout::Quad
                                                                : GridLayout::Single);
            } else {
                saveChannelSnapshot(channel);
            }
        });
        tb->addWidget(b);
    }
    toolbar->adjustSize();
    toolbar->hide();   // 호버 전까지 숨김
    tileToolbars_[channel] = toolbar;

    closeBtn->raise();
    toolbar->raise();
    layoutTileChrome(channel);
    return card;
}

// 카드 크기에 맞춰 크롬 위치를 다시 잡는다(레이아웃을 쓰지 않으므로 직접).
void MainWindow::layoutTileChrome(int channel)
{
    if (channel < 0 || channel >= 4) return;
    QWidget* card = videoCards[channel];
    if (!card) return;

    const int m = 6;
    if (auto* b = tileCloseBtns_[channel])
        b->move(card->width() - m - b->width(), m);
    if (auto* t = tileToolbars_[channel]) {
        t->adjustSize();
        t->move((card->width() - t->width()) / 2, card->height() - 8 - t->height());
    }
}

// 지금 보고 있는 화면 그대로 PNG로 저장한다(Wisenet 타일 스냅샷과 같은 동작).
void MainWindow::saveChannelSnapshot(int channel)
{
    if (channel < 0 || channel >= 4 || !channelViews[channel]) return;
    const QPixmap shot = channelViews[channel]->grab();
    if (shot.isNull()) return;

    const QString suggested =
        QStringLiteral("%1/CH%2_%3.png")
            .arg(QDir::homePath())
            .arg(channel + 1)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("스냅샷 저장"), suggested,
        QStringLiteral("PNG 이미지 (*.png)"));
    if (path.isEmpty()) return;
    if (!shot.save(path))
        QMessageBox::warning(this, QStringLiteral("스냅샷 저장"),
                             QStringLiteral("파일을 저장하지 못했습니다:\n%1").arg(path));
}

// 타일 하나를 레이아웃에서 빼거나 되돌린다. 마지막 한 장까지 숨기지는 못하게
// 막는다 — 영상이 하나도 없는 관제 화면은 아무 쓸모가 없다.
void MainWindow::setTileHidden(int ch, bool hidden)
{
    if (ch < 0 || ch >= 4) return;
    if (tileHidden_[ch] == hidden) return;
    if (hidden) {
        int visible = 0;
        for (int i = 0; i < 4; ++i)
            if (!tileHidden_[i]) ++visible;
        if (visible <= 1) return;
    }
    tileHidden_[ch] = hidden;
    if (hidden && selectedChannel_ == ch) {
        for (int i = 0; i < 4; ++i)
            if (!tileHidden_[i]) { selectChannel(i); break; }
    }
    relayoutGrid();
    refreshResourceTree();
}

void MainWindow::setGridLayout(GridLayout mode)
{
    gridLayout_ = mode;
    const int idx = mode == GridLayout::Quad ? 0
                  : mode == GridLayout::Spotlight ? 1 : 2;
    for (int i = 0; i < 3; ++i)
        if (layoutTabBtns_[i]) layoutTabBtns_[i]->setChecked(i == idx);
    relayoutGrid();
    refreshResourceTree();
}

// "지금 조작 대상" 채널을 바꾼다. 타일 테두리·리소스 트리 강조·타임라인이
// 전부 이 한 값을 따라간다 — 화면마다 다른 채널을 가리키면 오조작이 난다.
void MainWindow::selectChannel(int ch)
{
    if (ch < 0 || ch >= 4) return;
    // 빈 방을 보는 중에는 고를 채널이 없다. 타일 4장은 채널 0~3에 묶인 같은
    // 위젯이라, 막지 않으면 102호 타일을 눌렀는데 101호 채널이 선택되고
    // ROI 편집기까지 그 채널로 따라간다.
    // 경보 경로는 setVideoFocus()가 selectRoom(0)을 먼저 부르므로 막히지 않는다.
    if (selectedRoom_ != 0) return;
    if (tileHidden_[ch]) setTileHidden(ch, false);
    if (selectedChannel_ == ch && gridKey_ >= 0) return;
    selectedChannel_ = ch;

    for (int i = 0; i < 4; ++i) {
        if (!videoCards[i]) continue;
        videoCards[i]->setProperty("selected", i == ch);
        videoCards[i]->style()->unpolish(videoCards[i]);
        videoCards[i]->style()->polish(videoCards[i]);
    }
    // 스포트라이트/단일이면 큰 자리에 오는 채널 자체가 바뀐다.
    if (gridLayout_ != GridLayout::Quad) relayoutGrid();
    refreshResourceTree();
    refreshTimeline();   // 타임라인은 선택 채널의 녹화 구간을 보여준다

    // 녹화 재생 중 채널을 바꾸면 그 채널의 같은 시각으로 다시 불러온다.
    // 예전엔 타임라인만 갱신되고 실제 영상 소스는 안 바뀌어서, 채널만 바꾸고
    // ▶를 눌러도 이전 채널 화면에 대고 재생/정지만 토글될 뿐 반응이 없었다
    // (영상 로드는 seekPlaybackTo 한 곳뿐이라 타임라인을 직접 눌러야만 됐음).
    if (playbackMode_ && timeline_) seekPlaybackTo(timeline_->playhead());
}

// 배치의 유일한 결정 지점. gridLayout_ x selectedChannel_ x tileHidden_ 만 본다.
void MainWindow::relayoutGrid()
{
    if (!videoGrid) return;

    int hiddenMask = 0;
    for (int ch = 0; ch < 4; ++ch)
        if (tileHidden_[ch]) hiddenMask |= (1 << ch);
    const int key = int(gridLayout_) * 1000 + selectedChannel_ * 100 + hiddenMask;
    if (key == gridKey_) return;   // 같은 배치면 위젯을 건드리지 않는다(깜빡임 방지)
    gridKey_ = key;

    for (int ch = 0; ch < 4; ++ch)
        if (videoCards[ch]) videoGrid->removeWidget(videoCards[ch]);
    for (int i = 0; i < 4; ++i) {
        videoGrid->setColumnStretch(i, 0);
        videoGrid->setRowStretch(i, 0);
    }

    QVector<int> shown;
    for (int ch = 0; ch < 4; ++ch) {
        if (tileHidden_[ch]) {
            if (videoCards[ch]) videoCards[ch]->hide();
            continue;
        }
        shown.push_back(ch);
    }
    if (shown.isEmpty()) return;

    const int focus = tileHidden_[selectedChannel_] ? shown.first() : selectedChannel_;

    if (gridLayout_ == GridLayout::Single) {
        for (int ch : shown)
            if (videoCards[ch]) videoCards[ch]->setVisible(ch == focus);
        videoGrid->addWidget(videoCards[focus], 0, 0);
        videoGrid->setColumnStretch(0, 1);
        videoGrid->setRowStretch(0, 1);
        return;
    }

    for (int ch : shown)
        if (videoCards[ch]) videoCards[ch]->show();

    if (gridLayout_ == GridLayout::Spotlight && shown.size() > 1) {
        // 좌측 대형(전체 행 span) + 우측 나머지 세로 스택
        const int others = shown.size() - 1;
        videoGrid->addWidget(videoCards[focus], 0, 0, others, 1);
        int r = 0;
        for (int ch : shown) {
            if (ch == focus) continue;
            videoGrid->addWidget(videoCards[ch], r, 1, 1, 1);
            videoGrid->setRowStretch(r, 1);
            ++r;
        }
        videoGrid->setColumnStretch(0, 3);   // 대형 ~ 75%
        videoGrid->setColumnStretch(1, 1);
        return;
    }

    // 균등 격자 — 보이는 타일 수에 맞춰 열 수를 정한다(4·3·2 -> 2열, 1 -> 1열).
    const int cols = shown.size() <= 1 ? 1 : 2;
    for (int i = 0; i < shown.size(); ++i) {
        const int r = i / cols, c = i % cols;
        videoGrid->addWidget(videoCards[shown[i]], r, c);
        videoGrid->setColumnStretch(c, 1);
        videoGrid->setRowStretch(r, 1);
    }
}

// 경보용 스포트라이트 — 감지 채널을 크게, 나머지는 작게. channel<0 이면 2x2 복귀.
// 사용자가 고른 레이아웃을 덮어쓰지만, 경보는 그럴 자격이 있다.
void MainWindow::setVideoFocus(int channel)
{
    if (channel < 0 || channel >= 4) {
        setGridLayout(GridLayout::Quad);
        return;
    }
    // 경보는 전부 이 함수를 지나간다. 빈 방을 보고 있던 중이라면 실카메라 방으로
    // 되돌려야 한다 — 낙상이 났는데 화면엔 "카메라 미연결" 타일만 떠 있으면
    // 경보를 켜 놓고도 아무것도 못 보는 상태가 된다.
    selectRoom(0);
    selectChannel(channel);
    setGridLayout(GridLayout::Spotlight);
}

// ═══════════════════════════════════════════════════════════
//  스타일 (QSS)
// ═══════════════════════════════════════════════════════════
void MainWindow::applyTheme()
{
    // 달력 요일 머리글은 QSS가 아니라 코드로 칠하므로 팔레트 전환 때 직접 다시 준다.
    applyCalendarPalette(reportCalendar);
    // 적용 주체는 이 위젯이 아니라 qApp이다 — 이 창 하나가 아니라 앱 전체가
    // 같은 시트를 받아야 로그인 창까지 한 경로로 스타일이 흐른다.
    ThemeManager::applyStylesheet(darkMode ? kDark : kLight, darkMode);

    // statusDot·vitalDot·vitalBadge·statValue 등은 전부 severity/vital 동적
    // 속성 + QSS[severity=...]/[vital=...] 규칙이 색을 결정한다. 예전엔
    // 여기서 vitalStatusDots를 매번 흐리게 인라인으로 리셋했는데, 그 인라인
    // 값이 앱 전역 QSS 재적용에 지워지기 때문에 있던 보상 코드였다 — 이제
    // QSS가 속성 값을 스스로 기억하므로 리셋할 필요가 없다(02-03 Task2).
}

void MainWindow::toggleTheme()
{
    darkMode = !darkMode;
    applyPalette(darkMode ? kDark : kLight);
    applyTheme();     // 바뀐 팔레트로 QSS 재생성·재적용
    refreshNavIcons();  // 네비 아이콘은 QPainter로 그린 픽스맵이라 따로 다시 그린다

    if (themeToggleButton)
        themeToggleButton->setText(darkMode ? QStringLiteral("☀")
                                            : QStringLiteral("🌙"));

    // statusDot·vitalDot 등은 severity 동적 속성으로 색을 유지하므로
    // applyTheme() 뒤에 따로 복원할 필요가 없다.
    // 그래도 updateVitals()는 지우면 안 된다 — Sparkline은 QSS를 받지 않는
    // 커스텀 페인트 위젯이라 setLineColor()로 주입한 QColor를 그대로 들고
    // 있다. 여기서 다시 부르지 않으면 테마를 토글해도 스파크라인 선 색만
    // 이전 팔레트로 남는다.
    updateVitals();
    // 카드의 아바타/칩은 인라인 색이라 QSS 재적용만으론 안 바뀐다 → 다시 그린다.
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    // 도움말 본문은 인라인 색이라 QSS 재적용만으로는 안 바뀐다 — 열려 있으면 다시 그린다.
    if (helpBrowser && helpList)
        renderHelpTopic(helpList->currentRow());
}

void MainWindow::setConnectionState(bool connected, const QString& text)
{
    if (!statusDot) return;
    // collapsed/active 선례와 같은 4단계: 속성 설정 → unpolish → polish → update.
    // QSS 선택자가 문자열로 비교하는 값이므로 severity는 정확한 리터럴이어야 한다.
    statusDot->setProperty("severity", connected ? "normal" : "critical");
    statusDot->style()->unpolish(statusDot);
    statusDot->style()->polish(statusDot);
    statusDot->update();
    statusText->setText(text);
}

// ═══════════════════════════════════════════════════════════
//  실시간 시계 / 소켓 상태
// ═══════════════════════════════════════════════════════════
void MainWindow::updateClock()
{
    if (clockLabel)
        clockLabel->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd  HH:mm:ss"));
}

// 영상 서버 연결 상태가 바뀔 때마다(StreamClient::connectionStateChanged) 화면만 갱신한다.
// 접속·재접속·카메라 재전송 판단은 StreamClient가 한다.
void MainWindow::onStreamStateChanged()
{
    // 소켓이 여러 개(2-Pi)라 개별 state가 아니라 "전체 집계"로 상태 표시를 갱신한다.
    const int connected = stream_->connectedServerCount();
    if (connected == kNumServers)
        setConnectionState(true, QStringLiteral("영상 서버 연결됨"));
    else if (connected > 0)
        setConnectionState(false, QStringLiteral("영상 서버 %1/%2 연결")
                                      .arg(connected).arg(kNumServers));
    else
        setConnectionState(false, QStringLiteral("영상 서버 접속 중..."));

    // 담당 Pi가 끊긴 채널의 LIVE 표시등 소등
    for (int ch = 0; ch < 4; ++ch) {
        if (channelViews[ch] && !stream_->isChannelConnected(ch))
            channelViews[ch]->setLive(false);
    }
}

// 메인 화면 채널 배지(LIVE/미연결)를 소켓 상태만으로 못 잡는 경우까지 정확하게
// 만든다: Pi와의 TCP는 멀쩡한데 카메라 쪽 RTSP만 죽으면(케이블 빠짐 등) 새
// dbj_vs_header_t가 안 와서 setLive(true)가 다시 불릴 일이 없다 — 배지는 마지막
// 프레임 그대로 LIVE에 멈춰 있고, 관제사는 채널이 죽은 걸 설정 탭을 열어보기
// 전엔 알 방법이 없었다. 여기서 kChannelStaleTimeoutMs 동안 프레임이 없으면
// 배지를 직접 내린다.
void MainWindow::checkChannelHealth()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int ch = 0; ch < 4; ++ch) {
        if (!channelViews[ch] || !cameraActive_[ch]) continue;
        if (!channelViews[ch]->live()) continue;          // 이미 미연결 표시 중
        if (lastFrameMs_[ch] == 0) continue;               // 이번 세션 첫 프레임 전
        if (now - lastFrameMs_[ch] > kChannelStaleTimeoutMs) {
            channelViews[ch]->setLive(false);
            refreshResourceTree();   // 트리의 LIVE 색도 같이 식힌다
        }
    }
}
