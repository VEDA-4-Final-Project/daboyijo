#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "videoview.h"
#include <QHostAddress>
#include <QPixmap>
#include <QDateTime>
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QScrollArea>
#include <QRandomGenerator>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QDateEdit>
#include <QSlider>
#include <QStyle>
#include <QSplitter>
#include <QGroupBox>
#include <QFormLayout>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QUrl>
#include <QMouseEvent>
#include <QStackedWidget>

// ── 디자인 토큰 (다크 관제 테마) ─────────────────────────────
namespace {
// ── 밝은 의료 톤 팔레트 (요양원 주간 관제 환경) ──────────────
const char* kBgDeep      = "#F4F7FA"; // 최하단 배경(연회색)
const char* kPanel       = "#FFFFFF"; // 패널 배경(흰색)
const char* kCard        = "#F0F4F8"; // 카드 배경(연회색, 패널과 대비)
const char* kBorder      = "#DCE4EC"; // 테두리(연한 회색)
const char* kTextMain    = "#1E2A32"; // 기본 글자(진회색)
const char* kTextSub     = "#5C6B78"; // 보조 글자(중간 회색)
const char* kAccent      = "#12B5A6"; // 브랜드 강조(청록)
const char* kNormal      = "#2E9E5B"; // 정상(그린)
const char* kWarn        = "#C77A11"; // 주의(주황, 흰 배경 가독성 확보)
const char* kCritical    = "#E5484D"; // 위험(빨강)

// 상태 색상: 정상/주의/위험 판정
QString vitalColor(double temp, int hr) {
    if (temp >= 38.0 || hr >= 110 || hr <= 45) return kCritical;
    if (temp >= 37.5 || hr >= 100 || hr < 55)  return kWarn;
    return kNormal;
}

// 영상 서버 접속 정보 (RPi 주소) — TODO: 설정 파일/실행 인자로 분리
const char* kServerHost = "172.20.32.59";
constexpr quint16 kServerPort = 5500;
constexpr int kReconnectDelayMs = 3000;   // 끊김 후 재접속 간격

// 블랙박스 클립 HTTP 서버 포트 (server/src/main.cpp의 kClipHttpPort와 동일하게 유지)
constexpr quint16 kClipHttpPort = 5501;

// 블랙박스 재생 실패(=서버가 아직 파일 저장 중) 시 재시도 간격·횟수.
// 서버는 낙상 후 post초(현재 5초)를 더 녹화한 뒤에야 파일을 완성하므로,
// 낙상 직후 로그를 누르면 아직 파일이 없다 — 잠깐 기다렸다 다시 시도한다.
constexpr int kClipRetryDelayMs = 1500;
constexpr int kClipMaxRetries = 8;   // 약 12초까지 재시도

// 밀리초를 "mm:ss" 문자열로.
QString formatMs(qint64 ms) {
    if (ms < 0) ms = 0;
    const qint64 totalSec = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSec / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSec % 60, 2, 10, QLatin1Char('0'));
}

// 재생바 트랙의 아무 지점이나 좌클릭하면 그 위치로 바로 점프하는 슬라이더.
// (기본 QSlider는 트랙 클릭 시 pageStep만큼만 이동해 정확한 탐색이 어렵다
//  QProxyStyle의 SH_Slider_AbsoluteSetButtons 방식은 이 앱처럼 스타일시트를
//  쓰면 무시되는 Qt 제약이 있어, 마우스 이벤트를 직접 처리한다.)
class ClickSeekSlider : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && maximum() > minimum()) {
            // 클릭 지점으로 즉시 이동한 뒤 드래그 추적 시작.
            // (setSliderPosition을 먼저 해야 sliderPressed 수신 측에서
            //  value()가 이미 클릭 지점을 가리킨다 — 즉시 탐색용)
            setSliderPosition(valueFromX(e->position().toPoint().x()));
            setSliderDown(true);   // sliderPressed 발생
            e->accept();
            return;
        }
        QSlider::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (isSliderDown()) {
            setSliderPosition(valueFromX(e->position().toPoint().x()));
            e->accept();
            return;
        }
        QSlider::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && isSliderDown()) {
            setSliderPosition(valueFromX(e->position().toPoint().x()));
            setSliderDown(false);  // sliderReleased 발생
            e->accept();
            return;
        }
        QSlider::mouseReleaseEvent(e);
    }

private:
    int valueFromX(int x) const {
        return QStyle::sliderValueFromPosition(minimum(), maximum(), x, width(),
                                               invertedAppearance());
    }
};

// JPEG 페이로드 크기 상한 — 960x540 q80 실측 수십 KB 수준이라 4MB면 충분.
// 이걸 넘는 payload_len은 스트림 오염(또는 프로토콜 불일치)으로 본다.
constexpr quint32 kMaxPayloadLen = 4 * 1024 * 1024;

// 🌟 낙상 경보 해제 통신 프로토콜 상수 (0x03)
constexpr uint8_t kCtrlFallConfirm = 0x03;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 병상별 환자 정보 (실제 환자 DB 연동 지점)
    patients[0] = { QStringLiteral("전승현"), QStringLiteral("201호-1") };
    patients[1] = { QStringLiteral("박민용"), QStringLiteral("201호-2") };
    patients[2] = { QStringLiteral("이교민"), QStringLiteral("201호-3") };
    patients[3] = { QStringLiteral("김예훈"), QStringLiteral("201호-4") };

    buildUi();
    applyTheme();

    // 2. 소켓 생성 및 시그널 연결
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);
    connect(socket, &QTcpSocket::stateChanged, this, &MainWindow::onSocketStateChanged);

    // 3. 명세서 스펙: 5500번 포트로 즉시 접속 (IP 주소는 RPi 주소 입력)
    //    끊기면 reconnectTimer가 kReconnectDelayMs 후 재접속 (24시간 무인 관제용)
    reconnectTimer.setSingleShot(true);
    reconnectTimer.setInterval(kReconnectDelayMs);
    connect(&reconnectTimer, &QTimer::timeout, this, &MainWindow::connectToServer);
    connectToServer();

    // 4. 상단 시계 / 웨어러블 바이탈 타이머 가동
    connect(&clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    clockTimer.start(1000);
    updateClock();

    connect(&vitalsTimer, &QTimer::timeout, this, &MainWindow::updateVitals);
    vitalsTimer.start(2000);
    updateVitals();
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ═══════════════════════════════════════════════════════════
//  UI 빌드
// ═══════════════════════════════════════════════════════════
void MainWindow::buildUi()
{
    auto* root = new QVBoxLayout(ui->centralwidget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    tabWidget = new QTabWidget();
    tabWidget->setObjectName("mainTabs");

    // ── TAB 1: 실시간 관제 및 제어 (영상월 + 바이탈 패널) ──
    auto* body = new QHBoxLayout();
    body->setContentsMargins(16, 16, 16, 16);
    body->setSpacing(16);
    body->addWidget(buildVideoWall(), 7);
    body->addWidget(buildVitalsPanel(), 3);

    auto* dashboardTab = new QWidget();
    dashboardTab->setLayout(body);
    tabWidget->addTab(dashboardTab, QStringLiteral("실시간 관제 및 제어"));

    // ── TAB 2: 비상 로그 조회 및 블랙박스 ──
    tabWidget->addTab(buildLogArchiveTab(), QStringLiteral("비상 로그 조회 및 블랙박스"));

    // ── TAB 3: DB 관리 ──
    tabWidget->addTab(buildDbTab(), QStringLiteral("DB 관리"));

    root->addWidget(tabWidget, 1);

    resize(1280, 800);
    setMinimumSize(1080, 680);
}

QWidget* MainWindow::buildHeader()
{
    auto* header = new QFrame();
    header->setObjectName("header");
    header->setFixedHeight(64);

    auto* lay = new QHBoxLayout(header);
    lay->setContentsMargins(20, 0, 20, 0);
    lay->setSpacing(14);

    // 로고 / 타이틀
    auto* logo = new QLabel(QStringLiteral("다보이조"));
    logo->setObjectName("logo");
    auto* subtitle = new QLabel(QStringLiteral("요양원 통합 모니터링 · 201호"));
    subtitle->setObjectName("subtitle");

    lay->addWidget(logo);
    lay->addWidget(subtitle);
    lay->addStretch();

    // 연결 상태
    statusDot = new QLabel();
    statusDot->setObjectName("statusDot");
    statusDot->setFixedSize(10, 10);
    statusText = new QLabel();
    statusText->setObjectName("statusText");
    lay->addWidget(statusDot);
    lay->addWidget(statusText);

    // 구분선
    auto* sep = new QFrame();
    sep->setFrameShape(QFrame::VLine);
    sep->setObjectName("headerSep");
    sep->setFixedHeight(28);
    lay->addWidget(sep);

    // 실시간 시계
    clockLabel = new QLabel();
    clockLabel->setObjectName("clock");
    lay->addWidget(clockLabel);

    return header;
}

QWidget* MainWindow::buildVideoWall()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(12);

    // 제목 줄: 좌측 타이틀 + 우측 도구 (ROI / 인터콤 / 경보해제)
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("실시간 영상  ·  4채널"));
    title->setObjectName("panelTitle");
    titleRow->addWidget(title);
    titleRow->addStretch();

    roiButton = new QPushButton(QStringLiteral("ROI 지정"));
    roiButton->setObjectName("roiButton");
    roiButton->setCursor(Qt::PointingHandCursor);
    connect(roiButton, &QPushButton::clicked, this, &MainWindow::onRoiButtonClicked);
    titleRow->addWidget(roiButton);

    roiClearButton = new QPushButton(QStringLiteral("ROI 제거"));
    roiClearButton->setObjectName("roiClear");
    roiClearButton->setCursor(Qt::PointingHandCursor);
    connect(roiClearButton, &QPushButton::clicked, this, &MainWindow::onRoiClearClicked);
    titleRow->addWidget(roiClearButton);

    roiToggleButton = new QPushButton(QStringLiteral("ROI 표시"));
    roiToggleButton->setObjectName("roiToggle");
    roiToggleButton->setCheckable(true);
    roiToggleButton->setChecked(true);
    roiToggleButton->setCursor(Qt::PointingHandCursor);
    connect(roiToggleButton, &QPushButton::toggled, this,
            &MainWindow::onRoiVisibilityToggled);
    titleRow->addWidget(roiToggleButton);

    // 🎤 원격 방송(인터콤)
    micButton = new QPushButton(QStringLiteral("🎤 방송"));
    micButton->setObjectName("micButton");
    micButton->setCursor(Qt::PointingHandCursor);
    connect(micButton, &QPushButton::pressed, this, &MainWindow::onMicPressed);
    connect(micButton, &QPushButton::released, this, &MainWindow::onMicReleased);
    titleRow->addWidget(micButton);

    // 🚨 경보 해제
    alarmClearButton = new QPushButton(QStringLiteral("경보 해제"));
    alarmClearButton->setObjectName("alarmButton");
    alarmClearButton->setCursor(Qt::PointingHandCursor);
    connect(alarmClearButton, &QPushButton::clicked, this,
            &MainWindow::onAlarmClearClicked);
    titleRow->addWidget(alarmClearButton);

    outer->addLayout(titleRow);

    auto* grid = new QGridLayout();
    grid->setSpacing(12);
    grid->addWidget(buildVideoCard(0), 0, 0);
    grid->addWidget(buildVideoCard(1), 0, 1);
    grid->addWidget(buildVideoCard(2), 1, 0);
    grid->addWidget(buildVideoCard(3), 1, 1);
    outer->addLayout(grid, 1);

    return panel;
}

QWidget* MainWindow::buildVideoCard(int channel)
{
    auto* card = new QFrame();
    card->setObjectName("videoCard");
    card->setMinimumSize(320, 220);

    auto* lay = new QVBoxLayout(card);
    card->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // 상단 오버레이 바: 병상/환자 + LIVE
    auto* bar = new QFrame();
    bar->setObjectName("videoBar");
    bar->setFixedHeight(34);
    auto* barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(10, 0, 10, 0);
    barLay->setSpacing(8);

    auto* bed = new QLabel(patients[channel].bed);
    bed->setObjectName("bedBadge");
    auto* name = new QLabel(patients[channel].name);
    name->setObjectName("bedName");
    barLay->addWidget(bed);
    barLay->addWidget(name);
    barLay->addStretch();

    liveDots[channel] = new QLabel();
    liveDots[channel]->setObjectName("liveDotOff");
    liveDots[channel]->setFixedSize(8, 8);
    auto* liveTxt = new QLabel(QStringLiteral("LIVE"));
    liveTxt->setObjectName("liveText");
    barLay->addWidget(liveDots[channel]);
    barLay->addWidget(liveTxt);

    lay->addWidget(bar);

    // 영상 영역 — VideoView가 프레임 표시 + ROI 오버레이/그리기를 담당
    auto* video = new VideoView(channel);
    video->setObjectName("video");
    channelViews[channel] = video;
    connect(video, &VideoView::roiCompleted, this, &MainWindow::onRoiCompleted);
    connect(video, &VideoView::drawModeChanged, this,
            [this](int, bool on) {
                roiDrawing = on;
                if (roiButton)
                    roiButton->setText(on ? QStringLiteral("그리는 중… (취소하려면 다시 클릭)")
                                          : QStringLiteral("ROI 지정"));
            });
    lay->addWidget(video, 1);

    return card;
}

QWidget* MainWindow::buildVitalsPanel()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("웨어러블 생체신호"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 스크롤 가능한 바이탈 카드 목록
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName("vitalScroll");

    auto* inner = new QWidget();
    auto* list = new QVBoxLayout(inner);
    list->setContentsMargins(0, 0, 6, 0);
    list->setSpacing(12);
    for (int i = 0; i < 4; ++i)
        list->addWidget(buildVitalCard(i));
    list->addStretch();

    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    return panel;
}

QWidget* MainWindow::buildVitalCard(int channel)
{
    auto* card = new QFrame();
    card->setObjectName("vitalCard");

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(10);

    // 헤더: 상태등 + 이름 + 병상
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    vitalStatusDots[channel] = new QLabel();
    vitalStatusDots[channel]->setObjectName("vitalDot");
    vitalStatusDots[channel]->setFixedSize(10, 10);
    auto* name = new QLabel(patients[channel].name);
    name->setObjectName("vitalName");
    auto* bed = new QLabel(patients[channel].bed);
    bed->setObjectName("vitalBed");
    head->addWidget(vitalStatusDots[channel]);
    head->addWidget(name);
    head->addStretch();
    head->addWidget(bed);
    lay->addLayout(head);

    // 바이탈 값: 체온 / 심박수
    auto* stats = new QHBoxLayout();
    stats->setSpacing(10);

    auto makeStat = [&](const QString& icon, const QString& caption, QLabel*& valueRef) {
        auto* box = new QFrame();
        box->setObjectName("statBox");
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(12, 10, 12, 10);
        bl->setSpacing(2);
        auto* cap = new QLabel(icon + QStringLiteral("  ") + caption);
        cap->setObjectName("statCaption");
        valueRef = new QLabel(QStringLiteral("--"));
        valueRef->setObjectName("statValue");
        bl->addWidget(cap);
        bl->addWidget(valueRef);
        return box;
    };

    stats->addWidget(makeStat(QStringLiteral("🌡"), QStringLiteral("체온"), tempValues[channel]));
    stats->addWidget(makeStat(QStringLiteral("❤"), QStringLiteral("심박수"), hrValues[channel]));
    lay->addLayout(stats);

    // 갱신 시각
    vitalUpdated[channel] = new QLabel(QStringLiteral("웨어러블 연결 대기"));
    vitalUpdated[channel]->setObjectName("vitalUpdated");
    lay->addWidget(vitalUpdated[channel]);

    return card;
}

// ═══════════════════════════════════════════════════════════
//  TAB2: 비상 로그 조회 및 블랙박스
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildLogArchiveTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(12);

    outer->addWidget(buildSearchFilters());

    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildLogTable(), 6);

    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(16);
    rightCol->addWidget(buildBlackboxPlayer());
    rightCol->addWidget(buildCareTimeDashboard(), 1);
    auto* rightWrap = new QWidget();
    rightWrap->setLayout(rightCol);
    body->addWidget(rightWrap, 4);

    outer->addLayout(body, 1);
    return panel;
}

QWidget* MainWindow::buildSearchFilters()
{
    auto* bar = new QFrame();
    bar->setObjectName("filterBar");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);

    filterDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    filterDateFrom->setCalendarPopup(true);
    filterDateTo = new QDateEdit(QDate::currentDate());
    filterDateTo->setCalendarPopup(true);

    filterRoom = new QComboBox();
    filterRoom->addItems({QStringLiteral("전체 병실"), QStringLiteral("201호-1"),
                          QStringLiteral("201호-2"), QStringLiteral("201호-3"),
                          QStringLiteral("201호-4")});

    filterEventType = new QComboBox();
    filterEventType->addItems({QStringLiteral("전체 이벤트"), QStringLiteral("낙상"),
                               QStringLiteral("침상이탈"), QStringLiteral("보호사 진입")});

    auto* searchBtn = new QPushButton(QStringLiteral("검색"));
    searchBtn->setObjectName("roiButton");
    searchBtn->setCursor(Qt::PointingHandCursor);
    connect(searchBtn, &QPushButton::clicked, this, &MainWindow::onSearchClicked);

    lay->addWidget(new QLabel(QStringLiteral("날짜")));
    lay->addWidget(filterDateFrom);
    lay->addWidget(new QLabel(QStringLiteral("~")));
    lay->addWidget(filterDateTo);
    lay->addWidget(new QLabel(QStringLiteral("병실")));
    lay->addWidget(filterRoom);
    lay->addWidget(new QLabel(QStringLiteral("이벤트")));
    lay->addWidget(filterEventType);
    lay->addStretch();
    lay->addWidget(searchBtn);
    return bar;
}

QWidget* MainWindow::buildLogTable()
{
    logTable = new QTableWidget(0, 4);
    logTable->setObjectName("logTable");
    logTable->setHorizontalHeaderLabels(
        {QStringLiteral("날짜/시간"), QStringLiteral("병실"),
         QStringLiteral("이벤트"), QStringLiteral("상태")});
    logTable->horizontalHeader()->setStretchLastSection(true);
    logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(logTable, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::onLogRowActivated);
    return logTable;
}

QWidget* MainWindow::buildBlackboxPlayer()
{
    auto* card = new QFrame();
    card->setObjectName("videoCard");
    card->setMinimumHeight(220);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    blackboxPlaceholder = new QLabel(
        QStringLiteral("로그를 더블클릭하면\n10초 블랙박스 영상이 재생됩니다"));
    blackboxPlaceholder->setAlignment(Qt::AlignCenter);
    blackboxPlaceholder->setObjectName("video");

    blackboxVideoWidget = new QVideoWidget();
    blackboxVideoWidget->setObjectName("video");

    // placeholder ↔ 영상은 hide/show 대신 스택으로 전환한다.
    // hide/show 방식은 두 위젯의 선호 크기가 다를 때(특히 QVideoWidget은
    // 영상 원본 해상도를 선호 크기로 보고) 전환·로드 때마다 카드가
    // 커졌다 작아졌다 하는 원인이 된다. 스택 + Ignored 정책으로 내용물이
    // 레이아웃 크기에 영향을 못 주게 고정한다.
    blackboxStack = new QStackedWidget();
    blackboxStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    blackboxStack->addWidget(blackboxPlaceholder);
    blackboxStack->addWidget(blackboxVideoWidget);
    blackboxStack->setCurrentWidget(blackboxPlaceholder);
    lay->addWidget(blackboxStack, 1);

    // [재생/일시정지] + 재생바 + 시간(현재/전체) 한 줄
    auto* seekRow = new QHBoxLayout();
    seekRow->setSpacing(8);

    blackboxPlayPauseButton = new QPushButton();
    blackboxPlayPauseButton->setEnabled(false);
    blackboxPlayPauseButton->setCursor(Qt::PointingHandCursor);
    blackboxPlayPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    seekRow->addWidget(blackboxPlayPauseButton);

    blackboxSeek = new ClickSeekSlider(Qt::Horizontal);
    blackboxSeek->setEnabled(false);
    blackboxSeek->setRange(0, 0);   // duration 확정 전엔 0
    seekRow->addWidget(blackboxSeek, 1);

    blackboxTimeLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    blackboxTimeLabel->setObjectName("subtitle");
    // 시간 표시 폭을 고정해 재생 중 숫자 폭 변화로 재생바가 흔들리지 않게.
    blackboxTimeLabel->setMinimumWidth(
        blackboxTimeLabel->fontMetrics().horizontalAdvance(
            QStringLiteral("00:00 / 00:00")) + 8);
    seekRow->addWidget(blackboxTimeLabel);
    lay->addLayout(seekRow);

    blackboxPlayer = new QMediaPlayer(this);
    blackboxPlayer->setVideoOutput(blackboxVideoWidget);

    // 재생/일시정지 버튼 ↔ 플레이어 상태 동기화
    connect(blackboxPlayPauseButton, &QPushButton::clicked, this, [this] {
        if (blackboxPlayer->playbackState() == QMediaPlayer::PlayingState)
            blackboxPlayer->pause();
        else
            blackboxPlayer->play();
    });
    connect(blackboxPlayer, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState st) {
        blackboxPlayPauseButton->setIcon(style()->standardIcon(
            st == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause
                                             : QStyle::SP_MediaPlay));
    });

    // 클립 길이가 확정되면 슬라이더 범위를 ms 단위로 맞춘다(정확한 탐색).
    connect(blackboxPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        blackboxSeek->setRange(0, static_cast<int>(dur));
        blackboxTimeLabel->setText(formatMs(blackboxPlayer->position()) +
                                   QStringLiteral(" / ") + formatMs(dur));
    });
    // 재생 위치 변화 → 슬라이더/시간 갱신. 단, 사용자가 슬라이더를 잡고
    // 있는 동안은 덮어쓰지 않는다(안 그러면 드래그가 튕긴다).
    connect(blackboxPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        if (!blackboxSeeking)
            blackboxSeek->setValue(static_cast<int>(pos));
        blackboxTimeLabel->setText(formatMs(pos) + QStringLiteral(" / ") +
                                   formatMs(blackboxPlayer->duration()));
    });
    // 슬라이더 조작 → 탐색. 잡는 동안 positionChanged를 무시하기 위해 플래그 사용.
    // ClickSeekSlider가 press 시점에 value를 클릭 지점으로 먼저 옮겨두므로,
    // 누르는 순간 그 위치로 즉시 탐색된다(유튜브식 클릭 점프).
    connect(blackboxSeek, &QSlider::sliderPressed, this, [this] {
        blackboxSeeking = true;
        blackboxPlayer->setPosition(blackboxSeek->value());
    });
    connect(blackboxSeek, &QSlider::sliderReleased, this, [this] {
        blackboxPlayer->setPosition(blackboxSeek->value());
        blackboxSeeking = false;
    });
    connect(blackboxSeek, &QSlider::sliderMoved, this, [this](int v) {
        blackboxTimeLabel->setText(formatMs(v) + QStringLiteral(" / ") +
                                   formatMs(blackboxPlayer->duration()));
    });

    connect(blackboxPlayer, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& msg) {
        // 저장 완료 전에 눌렀으면 파일이 아직 없어 실패한다 — 몇 번 재시도.
        if (blackboxRetries < kClipMaxRetries && !blackboxUrl.isEmpty()) {
            ++blackboxRetries;
            blackboxPlaceholder->setText(
                QStringLiteral("블랙박스 저장 중… 잠시 후 재생됩니다 (%1)")
                    .arg(blackboxRetries));
            blackboxStack->setCurrentWidget(blackboxPlaceholder);
            const QString url = blackboxUrl;
            QTimer::singleShot(kClipRetryDelayMs, this, [this, url] {
                if (blackboxUrl == url) playBlackboxClip(url);
            });
            return;
        }
        blackboxPlaceholder->setText(
            QStringLiteral("재생 실패 — 클립을 찾을 수 없습니다\n(%1)").arg(msg));
        blackboxStack->setCurrentWidget(blackboxPlaceholder);
    });

    return card;
}

void MainWindow::playBlackboxClip(const QString& url)
{
    if (!blackboxPlayer) return;
    // 새 클립을 누른 것이면 재시도 카운터 초기화(같은 url 재시도면 유지).
    if (url != blackboxUrl) {
        blackboxUrl = url;
        blackboxRetries = 0;
    }
    blackboxStack->setCurrentWidget(blackboxVideoWidget);
    blackboxSeek->setEnabled(true);
    blackboxPlayPauseButton->setEnabled(true);

    // 같은 URL을 다시 setSource하면 QMediaPlayer가 "소스 변경 없음"으로 보고
    // 재로딩을 건너뛴다(특히 직전 재생이 끝까지 간 뒤). 그러면 같은 클립을
    // 연속으로 다시 틀 때 화면이 안 나온다 — 소스를 한 번 비웠다가 다시
    // 지정해 매번 확실히 처음부터 로드/재생되게 한다.
    blackboxPlayer->stop();
    blackboxPlayer->setSource(QUrl());
    blackboxPlayer->setSource(QUrl(url));
    blackboxPlayer->play();
}

QWidget* MainWindow::buildCareTimeDashboard()
{
    auto* card = new QFrame();
    card->setObjectName("vitalCard");

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("케어 타임 대시보드"));
    title->setObjectName("panelTitle");
    lay->addWidget(title);

    careTimeList = new QVBoxLayout();
    careTimeList->setSpacing(6);
    lay->addLayout(careTimeList);
    lay->addStretch();
    return card;
}

// ═══════════════════════════════════════════════════════════
//  TAB3: DB 관리
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildDbTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(16);

    // DB 연결 상태 표시 (멤버 변수에 할당 — 나중에 동적 갱신 가능)
    auto* statusBar = new QHBoxLayout();
    dbStatusDot = new QLabel();
    dbStatusDot->setObjectName("statusDot");
    dbStatusDot->setFixedSize(10, 10);
    dbStatusDot->setStyleSheet(QString("background:%1; border-radius:5px;").arg(kNormal));
    dbStatusText = new QLabel(QStringLiteral("DB 연결됨 · daboijo"));
    dbStatusText->setObjectName("statusText");

    auto* refreshBtn = new QPushButton(QStringLiteral("새로고침"));
    refreshBtn->setObjectName("roiButton");
    refreshBtn->setCursor(Qt::PointingHandCursor);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        refreshResidentTable();
        refreshCaregiverTable();
    });

    statusBar->addWidget(dbStatusDot);
    statusBar->addWidget(dbStatusText);
    statusBar->addStretch();
    statusBar->addWidget(refreshBtn);
    outer->addLayout(statusBar);

    // 입소자 섹션 + 요양사 섹션 상하 분할
    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(buildResidentSection());
    splitter->addWidget(buildCaregiverSection());
    splitter->setStretchFactor(0, 7);
    splitter->setStretchFactor(1, 3);
    outer->addWidget(splitter, 1);

    return panel;
}

QWidget* MainWindow::buildResidentSection()
{
    auto* card = new QFrame();
    card->setObjectName("vitalCard");

    auto* lay = new QHBoxLayout(card);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(16);

    // ── 좌측: 입소자 목록 ──
    auto* leftCol = new QVBoxLayout();
    auto* listTitle = new QLabel(QStringLiteral("입소자 목록"));
    listTitle->setObjectName("panelTitle");
    leftCol->addWidget(listTitle);

    residentTable = new QTableWidget(0, 8);
    residentTable->setObjectName("logTable");
    residentTable->setHorizontalHeaderLabels({
        QStringLiteral("ID"), QStringLiteral("이름"),
        QStringLiteral("병실"), QStringLiteral("침대"),
        QStringLiteral("채널"), QStringLiteral("웨어러블"),
        QStringLiteral("위험도"), QStringLiteral("상태")
    });
    residentTable->horizontalHeader()->setStretchLastSection(true);
    residentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    residentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    residentTable->setMinimumWidth(420);
    connect(residentTable, &QTableWidget::cellClicked,
            this, &MainWindow::onResidentSelected);
    leftCol->addWidget(residentTable, 1);

    auto* leftWrap = new QWidget();
    leftWrap->setLayout(leftCol);
    lay->addWidget(leftWrap, 5);

    // ── 우측: 상세/편집 폼 ──
    lay->addWidget(buildResidentForm(), 5);

    return card;
}

QWidget* MainWindow::buildResidentForm()
{
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* inner = new QWidget();
    auto* lay = new QVBoxLayout(inner);
    lay->setSpacing(12);
    lay->setContentsMargins(0, 0, 6, 0);

    auto makeGroup = [](const QString& title) {
        auto* g = new QGroupBox(title);
        g->setObjectName("formGroup");
        return g;
    };

    auto makeField = [](const QString& placeholder, QLineEdit*& ref) {
        ref = new QLineEdit();
        ref->setObjectName("formEdit");
        ref->setPlaceholderText(placeholder);
        return ref;
    };

    // ── 기본정보 ──
    auto* basicGroup = makeGroup(QStringLiteral("기본 정보"));
    auto* basicForm = new QFormLayout(basicGroup);
    basicForm->setSpacing(8);
    basicForm->setLabelAlignment(Qt::AlignLeft);
    basicForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    basicForm->addRow(QStringLiteral("이름"),        makeField("홍길동", editName));
    basicForm->addRow(QStringLiteral("병실"),        makeField("201", editRoom));
    basicForm->addRow(QStringLiteral("침대"),        makeField("A", editBed));
    basicForm->addRow(QStringLiteral("카메라 채널"), makeField("0~3", editCameraId));
    basicForm->addRow(QStringLiteral("웨어러블 ID"), makeField("기기 번호", editWearableId));
    lay->addWidget(basicGroup);

    // ── 케어 정보 ──
    auto* careGroup = makeGroup(QStringLiteral("케어 정보"));
    auto* careForm = new QFormLayout(careGroup);
    careForm->setSpacing(8);
    careForm->setLabelAlignment(Qt::AlignLeft);
    careForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    editCaregiver = new QComboBox();
    editCaregiver->setObjectName("formEdit");
    editCaregiver->addItem(QStringLiteral("(미지정)"));
    // TODO(core): SELECT name FROM caregivers WHERE status='재직' 으로 채우기
    careForm->addRow(QStringLiteral("담당 요양사"), editCaregiver);

    editRiskLevel = new QComboBox();
    editRiskLevel->setObjectName("formEdit");
    editRiskLevel->addItems({QStringLiteral("상"), QStringLiteral("중"), QStringLiteral("하")});
    careForm->addRow(QStringLiteral("위험도"), editRiskLevel);

    editAdmittedAt = new QDateEdit(QDate::currentDate());
    editAdmittedAt->setCalendarPopup(true);
    editAdmittedAt->setObjectName("formEdit");
    careForm->addRow(QStringLiteral("입원일"), editAdmittedAt);

    editDischargeDue = new QDateEdit(QDate::currentDate().addMonths(1));
    editDischargeDue->setCalendarPopup(true);
    editDischargeDue->setObjectName("formEdit");
    careForm->addRow(QStringLiteral("퇴원 예정일"), editDischargeDue);

    editStatus = new QComboBox();
    editStatus->setObjectName("formEdit");
    editStatus->addItems({QStringLiteral("퇴원"), QStringLiteral("재원")});
    careForm->addRow(QStringLiteral("상태"), editStatus);
    lay->addWidget(careGroup);

    // ── 보호자 정보 ──
    auto* guardianGroup = makeGroup(QStringLiteral("보호자 정보"));
    auto* guardianForm = new QFormLayout(guardianGroup);
    guardianForm->setSpacing(8);
    guardianForm->setLabelAlignment(Qt::AlignLeft);
    guardianForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    guardianForm->addRow(QStringLiteral("이름"),     makeField("보호자 이름", editGuardianName));
    guardianForm->addRow(QStringLiteral("관계"),     makeField("자녀/배우자 등", editGuardianRelation));
    guardianForm->addRow(QStringLiteral("전화번호"), makeField("010-0000-0000", editGuardianPhone));
    lay->addWidget(guardianGroup);

    // ── 특이사항 ──
    auto* notesGroup = makeGroup(QStringLiteral("특이사항"));
    auto* notesLay = new QVBoxLayout(notesGroup);
    editNotes = new QTextEdit();
    editNotes->setObjectName("formEdit");
    editNotes->setPlaceholderText(QStringLiteral("지병, 알레르기, 거동 가능 여부 등"));
    editNotes->setMaximumHeight(80);
    notesLay->addWidget(editNotes);
    lay->addWidget(notesGroup);

    // ── 버튼 ──
    auto* btnRow = new QHBoxLayout();
    auto* newBtn = new QPushButton(QStringLiteral("신규 등록"));
    newBtn->setObjectName("roiButton");
    newBtn->setCursor(Qt::PointingHandCursor);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::onNewResident);

    auto* saveBtn = new QPushButton(QStringLiteral("수정 저장"));
    saveBtn->setObjectName("roiButton");
    saveBtn->setCursor(Qt::PointingHandCursor);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveResident);

    auto* dischargeBtn = new QPushButton(QStringLiteral("퇴원 처리"));
    dischargeBtn->setObjectName("alarmButton");
    dischargeBtn->setCursor(Qt::PointingHandCursor);
    connect(dischargeBtn, &QPushButton::clicked, this, &MainWindow::onDischargeResident);

    btnRow->addWidget(newBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addStretch();
    btnRow->addWidget(dischargeBtn);
    lay->addLayout(btnRow);
    lay->addStretch();

    scroll->setWidget(inner);
    return scroll;
}

QWidget* MainWindow::buildCaregiverSection()
{
    auto* card = new QFrame();
    card->setObjectName("vitalCard");

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("요양사 관리"));
    title->setObjectName("panelTitle");
    lay->addWidget(title);

    caregiverTable = new QTableWidget(0, 5);
    caregiverTable->setObjectName("logTable");
    caregiverTable->setHorizontalHeaderLabels({
        QStringLiteral("ID"), QStringLiteral("이름"),
        QStringLiteral("연락처"), QStringLiteral("근무조"),
        QStringLiteral("상태")
    });
    caregiverTable->horizontalHeader()->setStretchLastSection(true);
    caregiverTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    caregiverTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(caregiverTable, 1);

    return card;
}

// ═══════════════════════════════════════════════════════════
//  스타일 (QSS)
// ═══════════════════════════════════════════════════════════
void MainWindow::applyTheme()
{
    const QString qss = QString(R"(
        QWidget { color: %(text); font-family: "Segoe UI", "맑은 고딕", sans-serif; font-size: 13px; }
        QMainWindow, #centralwidget { background: %(bgDeep); }

        #header { background: %(panel); border-bottom: 1px solid %(border); }
        #logo { color: %(accent); font-size: 20px; font-weight: 800; letter-spacing: 1px; }
        #subtitle { color: %(sub); font-size: 13px; }
        #statusText { color: %(sub); font-size: 12px; }
        #clock { color: %(text); font-size: 15px; font-weight: 600; }
        #headerSep { color: %(border); }

        #panel { background: %(panel); border: 1px solid %(border); border-radius: 12px; }
        #panelTitle { color: %(text); font-size: 15px; font-weight: 700; }

        #roiButton, #roiToggle, #roiClear { background: %(card); color: %(text); border: 1px solid %(border);
                                 border-radius: 8px; padding: 6px 14px; font-size: 12px; font-weight: 600; }
        #roiButton:hover, #roiToggle:hover { border-color: %(accent); }
        #roiToggle:checked { background: %(accent); color: #fff; border-color: %(accent); }
        #roiClear:hover { border-color: %(critical); color: %(critical); }

        #micButton { background: %(card); color: %(text); border: 1px solid %(border);
                     border-radius: 8px; padding: 6px 14px; font-size: 12px; font-weight: 600; }
        #micButton:hover { border-color: %(accent); }
        #micButton[active="true"] { background: %(critical); color: #fff; border-color: %(critical); }

        #alarmButton { background: %(critical); color: #fff; border: 1px solid %(critical);
                       border-radius: 8px; padding: 6px 14px; font-size: 12px; font-weight: 700; }
        #alarmButton:hover { background: #ff6b62; }

        #videoCard { background: #0B0F14; border: 1px solid %(border); border-radius: 10px; }
        #videoBar { background: %(panel); border-bottom: 1px solid %(border);
                    border-top-left-radius: 10px; border-top-right-radius: 10px; }
        #bedBadge { background: %(accent); color: #fff; font-size: 11px; font-weight: 700;
                    padding: 2px 8px; border-radius: 6px; }
        #bedName { color: %(text); font-size: 13px; font-weight: 600; }
        #liveText { color: %(sub); font-size: 11px; font-weight: 700; letter-spacing: 1px; }
        #video { color: #9AA7B2; font-size: 13px; background: #0B0F14;
                 border-bottom-left-radius: 10px; border-bottom-right-radius: 10px; }

        #vitalScroll { background: transparent; }
        #vitalScroll > QWidget > QWidget { background: transparent; }
        #vitalCard { background: %(card); border: 1px solid %(border); border-radius: 10px; }
        #vitalName { color: %(text); font-size: 15px; font-weight: 700; }
        #vitalBed { color: %(sub); font-size: 12px; }
        #statBox { background: %(panel); border: 1px solid %(border); border-radius: 8px; }
        #statCaption { color: %(sub); font-size: 11px; }
        #statValue { font-size: 22px; font-weight: 800; }
        #vitalUpdated { color: %(sub); font-size: 11px; }

        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar::handle:vertical { background: %(border); border-radius: 4px; min-height: 30px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; }

        /* ── TAB 구조 ── */
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: %(card); color: %(sub); padding: 10px 18px;
                       border: 1px solid %(border); border-bottom: none;
                       border-top-left-radius: 8px; border-top-right-radius: 8px; }
        QTabBar::tab:selected { background: %(panel); color: %(text); }
        QTabBar::tab:hover { color: %(text); }

        /* ── TAB2: 로그 조회 및 블랙박스 ── */
        #filterBar QLabel { color: %(sub); font-size: 12px; }
        #filterBar QComboBox, #filterBar QDateEdit {
            background: %(card); color: %(text); border: 1px solid %(border);
            border-radius: 6px; padding: 4px 8px; }
        #logTable { background: %(bgDeep); color: %(text); gridline-color: %(border);
                    border: 1px solid %(border); border-radius: 8px; }
        #logTable QHeaderView::section { background: %(card); color: %(sub);
                                         border: none; padding: 6px; }
        #logTable::item:selected { background: %(accent); color: #fff; }

/* ── TAB3: DB 관리 ── */

QLabel {
    color: %(text);
}

QGroupBox#formGroup {
    color: %(text);
    border: 1px solid %(border);
    border-radius: 8px;
    margin-top: 10px;
    padding: 14px 10px 10px 10px;
    font-weight: 700;
}

QGroupBox#formGroup::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 6px;
    color: %(text);
    background: %(card);
}

QGroupBox#formGroup QLabel {
    color: %(text);
    font-weight: 600;
    font-size: 13px;
    min-width: 90px;
}

QLineEdit#formEdit,
QTextEdit#formEdit,
QComboBox#formEdit,
QDateEdit#formEdit {
    background: %(panel);
    color: %(text);
    border: 1px solid %(border);
    border-radius: 6px;
    padding: 4px 8px;
}

QLineEdit#formEdit:focus,
QTextEdit#formEdit:focus {
    border-color: %(accent);
}

        QComboBox#formEdit QAbstractItemView {
            background: %(bgDeep); color: %(text);
            border: 1px solid %(border);
            selection-background-color: %(accent); selection-color: #fff; }
        QComboBox#formEdit::drop-down { border: none; width: 20px; }
        QComboBox#formEdit::down-arrow { image: none; width: 0; height: 0;
            border-left: 4px solid transparent; border-right: 4px solid transparent;
            border-top: 5px solid %(sub); margin-right: 8px; }

        QSplitter::handle { background: %(border); }

        /* ── 캘린더 팝업 (QDateEdit) ── */
        QCalendarWidget QWidget { background: %(panel); color: %(text); }
        QCalendarWidget QAbstractItemView {
            background: %(bgDeep); color: %(text);
            selection-background-color: %(accent); selection-color: #fff;
            outline: none; }
        QCalendarWidget QWidget#qt_calendar_navigationbar {
            background: %(card); border-bottom: 1px solid %(border); }
        QCalendarWidget QToolButton {
            background: transparent; color: %(text); border: none; padding: 4px 8px; }
        QCalendarWidget QToolButton:hover { background: %(border); border-radius: 4px; }
        QCalendarWidget QSpinBox {
            background: %(bgDeep); color: %(text); border: 1px solid %(border); }
        QCalendarWidget QAbstractItemView:disabled { color: %(sub); }
    )")
                            .replace("%(bgDeep)", kBgDeep)
                            .replace("%(panel)", kPanel)
                            .replace("%(card)", kCard)
                            .replace("%(border)", kBorder)
                            .replace("%(text)", kTextMain)
                            .replace("%(sub)", kTextSub)
                            .replace("%(accent)", kAccent)
                            .replace("%(critical)", kCritical);

    this->setStyleSheet(qss);

    // 상태등은 코드에서 배경색을 직접 지정 (동적 변경)
    statusDot->setStyleSheet(QString("background:%1; border-radius:5px;").arg(kCritical));
    for (int i = 0; i < 4; ++i) {
        liveDots[i]->setStyleSheet(QString("background:%1; border-radius:4px;").arg(kTextSub));
        vitalStatusDots[i]->setStyleSheet(QString("background:%1; border-radius:5px;").arg(kTextSub));
    }
}

void MainWindow::setConnectionState(bool connected, const QString& text)
{
    if (!statusDot) return;
    const char* color = connected ? kNormal : kCritical;
    statusDot->setStyleSheet(QString("background:%1; border-radius:5px;").arg(color));
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

void MainWindow::connectToServer()
{
    if (socket->state() != QAbstractSocket::UnconnectedState) return;
    setConnectionState(false, QStringLiteral("영상 서버 접속 중..."));
    buffer.clear();  // 이전 연결의 파싱 잔여물 폐기
    socket->connectToHost(QHostAddress(kServerHost), kServerPort);
    qDebug() << "영상 서버 접속 시도:" << kServerHost << ":" << kServerPort;
}

void MainWindow::onSocketStateChanged(QAbstractSocket::SocketState state)
{
    switch (state) {
    case QAbstractSocket::ConnectedState:
        setConnectionState(true, QStringLiteral("영상 서버 연결됨"));
        break;
    case QAbstractSocket::ConnectingState:
    case QAbstractSocket::HostLookupState:
        setConnectionState(false, QStringLiteral("영상 서버 접속 중..."));
        break;
    case QAbstractSocket::UnconnectedState:
    default:
        setConnectionState(false, QStringLiteral("영상 서버 연결 끊김 — 재접속 대기"));
        // 신호 끊긴 채널은 LIVE 표시등 소등
        for (int i = 0; i < 4; ++i)
            liveDots[i]->setStyleSheet(QString("background:%1; border-radius:4px;").arg(kTextSub));
        // 자동 재접속 예약 (스트림 오염으로 끊은 경우 포함)
        if (state == QAbstractSocket::UnconnectedState && !reconnectTimer.isActive())
            reconnectTimer.start();
        break;
    }
}

// ═══════════════════════════════════════════════════════════
//  웨어러블 바이탈 (현재는 시뮬레이션 — 실제 데이터 연동 지점)
// ═══════════════════════════════════════════════════════════
void MainWindow::updateVitals()
{
    auto* rng = QRandomGenerator::global();
    const QString now = QDateTime::currentDateTime().toString("HH:mm:ss");

    for (int i = 0; i < 4; ++i) {
        // 36.0~38.2℃ / 55~112bpm 범위로 자연스럽게 변동
        double temp = 36.0 + rng->bounded(220) / 100.0;
        int hr = 55 + rng->bounded(58);

        const QString color = vitalColor(temp, hr);

        tempValues[i]->setText(QString::number(temp, 'f', 1) + QStringLiteral(" ℃"));
        tempValues[i]->setStyleSheet(QString("color:%1;").arg(color));
        hrValues[i]->setText(QString::number(hr) + QStringLiteral(" bpm"));
        hrValues[i]->setStyleSheet(QString("color:%1;").arg(color));

        vitalStatusDots[i]->setStyleSheet(QString("background:%1; border-radius:5px;").arg(color));
        vitalUpdated[i]->setText(QStringLiteral("웨어러블 · 마지막 갱신 ") + now);
    }
}

// ═══════════════════════════════════════════════════════════
//  영상 수신 (명세서 프로토콜 파싱)
// ═══════════════════════════════════════════════════════════
void MainWindow::onReadyRead()
{
    // 🌟 명세서 가이드: 들어온 데이터를 무조건 글로벌 버퍼 뒤에 붙임
    buffer.append(socket->readAll());

    // 버퍼에 데이터가 남아있는 동안 무한 반복 파싱
    while (true) {
        // 0) 매직(2바이트)으로 패킷 종류 식별 — 영상(0xDB4B) / 이벤트(0xDB4D)
        if (buffer.size() < (int)sizeof(uint16_t))
            return;
        uint16_t magic;
        memcpy(&magic, buffer.constData(), sizeof(magic));

        // ── 이벤트 패킷 (낙상 통보 등, 페이로드 없음) ──
        if (magic == kEvtMagic) {
            if (buffer.size() < (int)sizeof(dbj_evt_header_t))
                return;  // 헤더가 덜 옴 — 다음 readyRead 대기
            dbj_evt_header_t evt;
            memcpy(&evt, buffer.constData(), sizeof(evt));
            buffer.remove(0, sizeof(evt));

            if (evt.channel < 4) {
                if (evt.type == kEvtFall) {
                    handleFallEvent(evt.channel, evt.timestamp_ms);
                }
                else if (evt.type == kEvtBedEgress) {
                    handleBedEgressEvent(evt.channel, evt.timestamp_ms);
                }
            }
            continue;
        }

        // ── 영상 프레임 패킷 ──
        // 1) 헤더 크기(16바이트)만큼도 안 모였으면 데이터 더 올 때까지 대기
        if (buffer.size() < (int)sizeof(dbj_vs_header_t))
            return;

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
            buffer.clear(); // 오염된 버퍼 초기화
            socket->abort(); // 즉시 끊기 → UnconnectedState → 재접속 타이머 가동
            return;
        }

        // 4) 전체 패킷 크기 계산 = 헤더(16B) + 진짜 JPEG 크기
        //    (qint64 — uint32 payload_len과의 int 오버플로우 방지)
        const qint64 total = static_cast<qint64>(sizeof(header)) + header.payload_len;

        // JPEG 데이터가 아직 다 안 왔으면 다음 readyRead 때까지 대기
        if (buffer.size() < total)
            return;

        // 5) 정확한 페이로드 위치와 크기만큼 지정하여 QImage 생성
        QImage image = QImage::fromData(
            reinterpret_cast<const uchar*>(buffer.constData()) + sizeof(header),
            header.payload_len,
            "JPEG"
            );

        // 6) 지연 시간(Latency) 모니터링
        qint64 current_time = QDateTime::currentMSecsSinceEpoch();
        qint64 latency = current_time - header.timestamp_ms;
        qDebug() << "Channel:" << header.channel << " | Latency:" << latency << "ms";

        // 7) 사용이 끝난 패킷만큼 버퍼 맨 앞에서 도려내기
        buffer.remove(0, static_cast<int>(total));

        // 8) channel 값으로 4분할 위젯 분배 및 렌더링
        if (!image.isNull()) {
            if (header.channel >= 0 && header.channel < 4) {
                channelViews[header.channel]->setFrame(QPixmap::fromImage(image));
                liveDots[header.channel]->setStyleSheet(
                    QString("background:%1; border-radius:4px;").arg(kCritical));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  낙상 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleFallEvent(int channel, quint64 timestampMs)
{
    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        fallActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(true);
        }
        qDebug() << "🚨 [낙상 감지] 채널" << channel << "빨간 테두리 켜짐 (모자이크 자동 해제 상태)";
    }

    // 2. 비상 로그 조회 탭에 URL 및 정보 등록
    if (logTable) {
        const int row = logTable->rowCount();
        logTable->insertRow(row);
        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 static_cast<qint64>(timestampMs)).toString("yyyy-MM-dd hh:mm:ss");
        auto* dtItem = new QTableWidgetItem(when);
        const QString clipUrl = QStringLiteral("http://%1:%2/ch%3_%4.mp4")
                                     .arg(QString::fromLatin1(kServerHost))
                                     .arg(kClipHttpPort)
                                     .arg(channel)
                                     .arg(timestampMs);
        dtItem->setData(Qt::UserRole, clipUrl);
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("낙상")));
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));
    }
}

// ═══════════════════════════════════════════════════════════
//  침상 이탈 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleBedEgressEvent(int channel, quint64 timestampMs)
{
    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        fallActive[channel] = true; // 경보 해제 로직과 연동하기 위해 활성화
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(true);
        }
        qDebug() << "⚠️ [침상 이탈 감지] 채널" << channel << "빨간 테두리 켜짐";
    }

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable) {
        const int row = logTable->rowCount();
        logTable->insertRow(row);
        
        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 static_cast<qint64>(timestampMs)).toString("yyyy-MM-dd hh:mm:ss");
        auto* dtItem = new QTableWidgetItem(when);
        
        const QString clipUrl = QStringLiteral("http://%1:%2/ch%3_%4.mp4")
                                     .arg(QString::fromLatin1(kServerHost))
                                     .arg(kClipHttpPort)
                                     .arg(channel)
                                     .arg(timestampMs);          
        dtItem->setData(Qt::UserRole, clipUrl);
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("침상 이탈"))); // 💡 이탈 분류로 등록
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));
    }
}

// ═══════════════════════════════════════════════════════════
//  침대 ROI 지정 / 표시 / 서버 전송
// ═══════════════════════════════════════════════════════════
void MainWindow::onRoiButtonClicked()
{
    // 이미 그리는 중이면 이 버튼은 "취소"로 동작
    if (roiDrawing) {
        for (auto* v : channelViews)
            if (v && v->drawMode()) v->cancelDraft();
        return;
    }

    // 4채널 중 하나 선택 (병상/환자 이름으로 표기)
    QStringList items;
    for (int i = 0; i < 4; ++i)
        items << QStringLiteral("채널 %1  ·  %2 (%3)")
                     .arg(i + 1).arg(patients[i].name, patients[i].bed);

    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, QStringLiteral("ROI 지정"),
        QStringLiteral("침대 ROI를 그릴 채널을 선택하세요:"), items, 0, false, &ok);
    if (!ok) return;

    const int channel = items.indexOf(choice);
    if (channel < 0 || channel >= 4) return;

    channelViews[channel]->setDrawMode(true);  // 그리기 시작 (좌클릭=점, 더블클릭=완료)
}

void MainWindow::onRoiClearClicked()
{
    // 그리는 중이면 먼저 그 작업을 취소해야 헷갈리지 않는다
    if (roiDrawing) {
        for (auto* v : channelViews)
            if (v && v->drawMode()) v->cancelDraft();
    }

    // ROI가 실제로 설정된 채널만 후보로 제시
    QStringList items;
    QList<int> channels;
    for (int i = 0; i < 4; ++i) {
        if (channelViews[i] && !channelViews[i]->roi().isEmpty()) {
            items << QStringLiteral("채널 %1  ·  %2 (%3)")
                         .arg(i + 1).arg(patients[i].name, patients[i].bed);
            channels << i;
        }
    }

    if (items.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ROI 제거"),
                                QStringLiteral("제거할 ROI가 설정된 채널이 없습니다."));
        return;
    }

    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, QStringLiteral("ROI 제거"),
        QStringLiteral("ROI를 제거할 채널을 선택하세요:"), items, 0, false, &ok);
    if (!ok) return;

    const int idx = items.indexOf(choice);
    if (idx < 0) return;
    const int channel = channels[idx];

    if (QMessageBox::question(
            this, QStringLiteral("ROI 제거"),
            QStringLiteral("채널 %1의 침대 ROI를 제거할까요?").arg(channel + 1))
        != QMessageBox::Yes)
        return;

    sendRoi(channel, QPolygonF(), true);   // 서버에 삭제 통보
    channelViews[channel]->clearRoi();      // 로컬 오버레이 제거
    qDebug() << "ROI 제거: ch" << channel;
}

void MainWindow::onRoiVisibilityToggled(bool on)
{
    for (auto* v : channelViews)
        if (v) v->setRoiVisible(on);
    if (roiToggleButton)
        roiToggleButton->setText(on ? QStringLiteral("ROI 표시")
                                    : QStringLiteral("ROI 숨김"));
}

void MainWindow::onRoiCompleted(int channel, const QPolygonF& normPts)
{
    sendRoi(channel, normPts);
    // 방금 그린 걸 볼 수 있도록 표시 토글이 꺼져 있으면 켠다
    if (roiToggleButton && !roiToggleButton->isChecked())
        roiToggleButton->setChecked(true);
    qDebug() << "ROI 전송: ch" << channel << "," << normPts.size() << "점";
}

void MainWindow::sendRoi(int channel, const QPolygonF& normPts, bool clear)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("영상 서버에 연결되어 있지 않습니다."));
        return;
    }

    const int n = clear ? 0 : qMin(static_cast<int>(normPts.size()), 32);

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = clear ? kCtrlRoiClear : kCtrlRoiSet;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = static_cast<uint8_t>(n);
    h.reserved = 0;

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    for (int i = 0; i < n; ++i) {
        dbj_roi_point_t p;
        p.x = static_cast<uint16_t>(
            qBound(0.0, normPts[i].x() * kRoiCoordScale, double(kRoiCoordScale)));
        p.y = static_cast<uint16_t>(
            qBound(0.0, normPts[i].y() * kRoiCoordScale, double(kRoiCoordScale)));
        pkt.append(reinterpret_cast<const char*>(&p), sizeof(p));
    }
    socket->write(pkt);
    socket->flush();
}

// ═══════════════════════════════════════════════════════════
//  원격 방송(인터콤)
// ═══════════════════════════════════════════════════════════
void MainWindow::onMicPressed()
{
    micButton->setText(QStringLiteral("🔴 방송 중"));
    micButton->setProperty("active", true);
    micButton->style()->unpolish(micButton);
    micButton->style()->polish(micButton);
    qDebug() << "인터콤 방송 시작";
}

void MainWindow::onMicReleased()
{
    micButton->setText(QStringLiteral("🎤 방송"));
    micButton->setProperty("active", false);
    micButton->style()->unpolish(micButton);
    micButton->style()->polish(micButton);
    qDebug() << "인터콤 방송 종료";
}

// ═══════════════════════════════════════════════════════════
//  [경보 해제] 버튼 클릭 시 동작 (즉각적인 테두리 OFF + 마스크 ON 패킷 송신)
// ═══════════════════════════════════════════════════════════
void MainWindow::onAlarmClearClicked()
{
    bool packetSent = false;

    // 되묻는 팝업 없이 버튼 클릭 즉시 원스톱으로 리셋 처리!
    for (int channel = 0; channel < 4; ++channel) {
        if (fallActive[channel]) {
            // 1. 빨간 테두리 끄고 로컬 경보 상태 클리어
            fallActive[channel] = false;
            if (channelViews[channel]) {
                channelViews[channel]->setAlert(false);
            }

            // 2. 서버의 PrivacyMasker를 깨워서 다시 모자이크 씌우라고 0x03 바이너리 쏘기
            if (socket && socket->state() == QAbstractSocket::ConnectedState) {
                dbj_ctrl_header_t h;
                h.magic = kCtrlMagic;                  // 0xDB4C
                h.version = 0x01;
                h.type = kCtrlFallConfirm;             // 0x03 (경보 확인 -> 마스크 복구)
                h.channel = static_cast<uint8_t>(channel);
                h.point_count = 0;
                h.reserved = 0;

                socket->write(reinterpret_cast<const char*>(&h), sizeof(h));
                packetSent = true;
                qDebug() << "🔓 [Qt -> 서버] 채널" << channel << "경보 확인 및 모자이크 복구 패킷 전송!";
            }
        }
    }

    if (packetSent) {
        socket->flush(); // 버퍼 비우고 네트워크 선으로 즉시 방출
    } else {
        // 현재 활성화된 경보가 아예 없을 때만 안내 메시지 표시
        QMessageBox::information(this, QStringLiteral("경보 해제"),
                                 QStringLiteral("현재 활성화된 낙상 경보가 없습니다."));
    }
}

// ═══════════════════════════════════════════════════════════
//  TAB2 슬롯 — 자리표시자 (서버 연동 전)
// ═══════════════════════════════════════════════════════════
void MainWindow::onSearchClicked()
{
    qDebug() << "검색 조건 —"
             << filterDateFrom->date().toString("yyyy-MM-dd") << "~"
             << filterDateTo->date().toString("yyyy-MM-dd")
             << filterRoom->currentText() << filterEventType->currentText();
}

void MainWindow::onLogRowActivated(int row, int /*column*/)
{
    if (!logTable) return;
    auto* item = logTable->item(row, 0);
    const QString url = item ? item->data(Qt::UserRole).toString() : QString();
    if (url.isEmpty()) {
        qDebug() << "블랙박스 재생 요청 — row" << row << "(클립 URL 없음, DB 연동 전 로그로 추정)";
        return;
    }
    qDebug() << "블랙박스 재생 요청 —" << url;
    playBlackboxClip(url);
}

// ═══════════════════════════════════════════════════════════
//  TAB3 슬롯 — 자리표시자 (DB 쿼리 연동 전)
// ═══════════════════════════════════════════════════════════
void MainWindow::refreshResidentTable()
{
    residentTable->setRowCount(0);
    qDebug() << "입소자 목록 새로고침 (DB 연동 전)";
}

void MainWindow::refreshCaregiverTable()
{
    caregiverTable->setRowCount(0);
    qDebug() << "요양사 목록 새로고침 (DB 연동 전)";
}

void MainWindow::onResidentSelected(int row, int /*column*/)
{
    if (!residentTable || row < 0) return;
    auto* idItem = residentTable->item(row, 0);
    if (idItem) selectedResidentId = idItem->text().toInt();
    qDebug() << "입소자 선택 — ID:" << selectedResidentId;
}

void MainWindow::onNewResident()
{
    selectedResidentId = -1;
    editName->clear();
    editRoom->clear();
    editBed->clear();
    editCameraId->clear();
    editWearableId->clear();
    editGuardianName->clear();
    editGuardianPhone->clear();
    editGuardianRelation->clear();
    editNotes->clear();
    editRiskLevel->setCurrentIndex(1);
    editStatus->setCurrentIndex(1);
    editAdmittedAt->setDate(QDate::currentDate());
    editDischargeDue->setDate(QDate::currentDate().addMonths(1));
    editName->setFocus();
}

void MainWindow::onSaveResident()
{
    if (editName->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("이름을 입력해주세요."));
        return;
    }
    qDebug() << "입소자 저장 —"
             << "이름:" << editName->text()
             << "병실:" << editRoom->text()
             << "침대:" << editBed->text()
             << "위험도:" << editRiskLevel->currentText();
    QMessageBox::information(this, QStringLiteral("저장"),
                             QStringLiteral("저장되었습니다. (DB 연동 전 — 자리표시자)"));
} 

void MainWindow::onDischargeResident()
{
    if (selectedResidentId < 0) {
        QMessageBox::warning(this, QStringLiteral("선택 없음"),
                             QStringLiteral("퇴원 처리할 입소자를 먼저 선택해주세요."));
        return;
    }
    const auto reply = QMessageBox::question(
        this, QStringLiteral("퇴원 처리"),
        QStringLiteral("선택한 입소자를 퇴원 처리하시겠습니까?\n(기록은 보존됩니다)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    qDebug() << "퇴원 처리 — ID:" << selectedResidentId;
}
