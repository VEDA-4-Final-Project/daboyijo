#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "theme.h"
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
#include <QColor>
#include <QDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QSettings>
#include <QLineEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QUdpSocket>
#include <QUuid>
#include <QXmlStreamReader>
#include <QTimer>
#include <QSet>
#include <QRegularExpression>
#include <QAbstractItemView>
#include <QNetworkInterface>
#include <algorithm>

// 디자인 토큰(kLight/kDark/kAccent…)은 theme.h로 분리했다 — 로그인 화면과 공유.
namespace {

// 상태 색상: 정상/주의/위험 판정
QString vitalColor(double temp, int hr) {
    if (temp >= 38.0 || hr >= 110 || hr <= 45) return kCritical;
    if (temp >= 37.5 || hr >= 100 || hr < 55)  return kWarn;
    return kNormal;
}

// 상태 라벨: 정상/주의/위험 (배지 텍스트용)
QString vitalStatusLabel(double temp, int hr) {
    if (temp >= 38.0 || hr >= 110 || hr <= 45) return QStringLiteral("위험");
    if (temp >= 37.5 || hr >= 100 || hr < 55)  return QStringLiteral("주의");
    return QStringLiteral("정상");
}

// 두 색을 f:(1-f) 비율로 섞는다. 배지 배경 tint 계산용.
// (fg를 현재 카드색 bg와 섞으면 라이트/다크 어느 테마에서도 자연스러운 옅은 배경이 된다)
QString blendHex(const QString& fg, const QString& bg, double f) {
    QColor a(fg), b(bg);
    return QColor(int(a.red()   * f + b.red()   * (1 - f)),
                  int(a.green() * f + b.green() * (1 - f)),
                  int(a.blue()  * f + b.blue()  * (1 - f))).name();
}

// 영상 서버 접속 정보 (RPi 주소) — 2-Pi 분할: 채널을 두 라즈베리에 2+2로 나눠 서빙.
//   Pi A = ch0·ch1, Pi B = ch2·ch3. 각 Pi의 cameras.conf 채널 번호가 아래 인덱스와
//   일치해야 하고(MainWindow::serverForChannel), Qt는 두 IP에 각각 붙는다.
// 하드코딩 대신 QSettings(관제 PC 로컬)에 저장 — 기본값은 아래 상수이며, 관제
// PC마다 다른 Pi를 볼 수 있게 설정에서 바꿀 수 있다. (CCTV IP와는 별개: 이건 Qt가
// "붙는 서버" 주소이고, CCTV는 서버가 여는 카메라 주소다.)
namespace {
const char* kSettingsHostA = "server/hostA";     // Pi A (ch0·ch1)
const char* kSettingsHostB = "server/hostB";     // Pi B (ch2·ch3)
const char* kDefaultHostA  = "172.20.35.202";
const char* kDefaultHostB  = "172.20.35.201";

// 서버 인덱스(0=Pi A, 1=Pi B) → 저장된 호스트(없으면 기본값).
QString serverHost(int idx) {
    QSettings s;
    return idx == 0 ? s.value(kSettingsHostA, kDefaultHostA).toString()
                    : s.value(kSettingsHostB, kDefaultHostB).toString();
}
// 채널(0~3) → 담당 Pi의 호스트 (블랙박스 클립 URL 등 host가 필요한 곳용).
// 매핑은 MainWindow::serverForChannel과 동일하게 유지할 것 (ch0,1→0 / ch2,3→1).
QString hostForChannel(int ch) { return serverHost(ch < 2 ? 0 : 1); }
}  // namespace
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

// 통합 경보 해제 통신 프로토콜 상수 (0x03)
constexpr uint8_t kCtrlAlarmConfirm = 0x03;
}

MainWindow::MainWindow(const Auth::SessionUser& user, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , currentUser(user)
{
    ui->setupUi(this);

    // 병상별 환자 정보 (실제 환자 DB 연동 지점)
    patients[0] = { QStringLiteral("전승현"), QStringLiteral("201호-1") };
    patients[1] = { QStringLiteral("박민용"), QStringLiteral("201호-2") };
    patients[2] = { QStringLiteral("이교민"), QStringLiteral("201호-3") };
    patients[3] = { QStringLiteral("김예훈"), QStringLiteral("201호-4") };

    buildUi();
    applyTheme();

    // DB 입소자 목록 초기 로드 (main.cpp에서 연결을 이미 열어둠)
    refreshResidentTable();

    // 2. 서버별 소켓 생성 및 시그널 연결 (2-Pi: 소켓 kNumServers개)
    //    수신 슬롯 onReadyRead는 sender()로 어느 소켓이 신호를 냈는지 구분한다.
    for (int i = 0; i < kNumServers; ++i) {
        sockets[i] = new QTcpSocket(this);
        connect(sockets[i], &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);
        connect(sockets[i], &QTcpSocket::stateChanged, this, &MainWindow::onSocketStateChanged);
    }

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

    // 🚀 [블랙박스 복구] 각 Pi의 HTTP 서버(/list)에서 과거 클립 목록을 받아 병합한다.
    //    2-Pi: 서버마다 자기 채널 클립만 갖고 있으므로 양쪽에서 받아 테이블에 누적한다.
    if (logTable) logTable->setRowCount(0);  // 시작 전 1회만 비움 (응답들이 누적)
    for (int si = 0; si < kNumServers; ++si) {
        auto* manager = new QNetworkAccessManager(this);
        const QString host = serverHost(si);
        QUrl url(QStringLiteral("http://%1:%2/list").arg(host).arg(kClipHttpPort));
        QNetworkReply* reply = manager->get(QNetworkRequest(url));

        connect(reply, &QNetworkReply::finished, this, [this, reply, host]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "⚠️ 과거 영상 목록 수집 실패(" << host << "):" << reply->errorString();
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray() || !logTable) return;

            // 이 Pi가 준 목록을 테이블에 "추가"한다(비우지 않음 — 다른 Pi 것과 병합).
            logTable->setSortingEnabled(false);
            const QJsonArray fileList = doc.array();
            for (const QJsonValue& value : fileList) {
                const QString fileName = value.toString();
                // 확장자(.mp4) 제거 후 '_' 기준으로 채널/타임스탬프/유형 분리.
                // 서버 저장 규칙: 낙상은 _FALL, 침상이탈은 _EGRESS 접미사.
                const QString cleanName = fileName.left(fileName.lastIndexOf('.'));
                const QStringList parts = cleanName.split(QLatin1Char('_'));
                if (parts.size() < 2) continue;   // 최소 chN_타임스탬프 필요

                const int channel = parts[0].mid(2).toInt();   // "ch1" -> 1
                const qint64 timestampMs = parts[1].toLongLong();
                const QString rawType = (parts.size() >= 3) ? parts[2] : QString();
                if (channel < 0 || channel >= 4) continue;

                const QString eventType =
                    (rawType == QLatin1String("EGRESS")) ? QStringLiteral("침상 이탈")
                                                         : QStringLiteral("낙상");

                // 정렬(문자열 비교)이 시간순이 되도록 24시간(HH) 포맷 사용
                const QString when = QDateTime::fromMSecsSinceEpoch(timestampMs)
                                         .toString("yyyy-MM-dd HH:mm:ss");

                const int row = logTable->rowCount();
                logTable->insertRow(row);

                auto* dtItem = new QTableWidgetItem(when);
                // 이 목록을 준 Pi(host)에 그 클립이 있으므로 재생 URL도 그 host로.
                const QString clipUrl = QStringLiteral("http://%1:%2/%3")
                                             .arg(host).arg(kClipHttpPort).arg(fileName);
                dtItem->setData(Qt::UserRole, clipUrl);

                logTable->setItem(row, 0, dtItem);
                logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
                logTable->setItem(row, 2, new QTableWidgetItem(eventType));
                logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));
            }
            // 채우기 후 정렬 활성화 → 전체(양쪽 Pi) 최신순 재정렬 (0열 시간 문자열)
            logTable->setSortingEnabled(true);
            logTable->sortItems(0, Qt::DescendingOrder);
            qDebug() << "✅ 블랙박스 복원 (" << host << "총" << fileList.size() << "개)";
        });
    }
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
    body->addWidget(buildVideoWall(), 1);
    body->addWidget(buildVitalsPanel(), 0);

    auto* dashboardTab = new QWidget();
    dashboardTab->setLayout(body);
    tabWidget->addTab(dashboardTab, QStringLiteral("실시간 관제 및 제어"));

    // ── TAB 2: 비상 로그 조회 및 블랙박스 ──
    tabWidget->addTab(buildLogArchiveTab(), QStringLiteral("비상 로그 조회 및 블랙박스"));

    // ── TAB 3: DB 관리 ──
    tabWidget->addTab(buildDbTab(), QStringLiteral("DB 관리"));

    root->addWidget(tabWidget, 1);

    resize(1600, 940);
    setMinimumSize(1340, 760);
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

    // 연결 상태 — pill 배지
    auto* statusPill = new QFrame();
    statusPill->setObjectName("statusPill");
    auto* spLay = new QHBoxLayout(statusPill);
    spLay->setContentsMargins(9, 2, 10, 2);
    spLay->setSpacing(6);
    statusDot = new QLabel();
    statusDot->setObjectName("statusDot");
    statusDot->setFixedSize(7, 7);
    statusText = new QLabel();
    statusText->setObjectName("statusText");
    spLay->addWidget(statusDot);
    spLay->addWidget(statusText);
    lay->addWidget(statusPill);

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

    // 구분선 + 테마 토글
    auto* sep2 = new QFrame();
    sep2->setFrameShape(QFrame::VLine);
    sep2->setObjectName("headerSep");
    sep2->setFixedHeight(28);
    lay->addWidget(sep2);

    themeToggleButton = new QPushButton(QStringLiteral("🌙"));
    themeToggleButton->setObjectName("themeToggle");
    themeToggleButton->setCursor(Qt::PointingHandCursor);
    themeToggleButton->setToolTip(QStringLiteral("라이트/다크 테마 전환"));
    connect(themeToggleButton, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    lay->addWidget(themeToggleButton);

    // 구분선 + 로그인 사용자 / 로그아웃
    auto* sep3 = new QFrame();
    sep3->setFrameShape(QFrame::VLine);
    sep3->setObjectName("headerSep");
    sep3->setFixedHeight(28);
    lay->addWidget(sep3);

    // 이름 첫 글자를 딴 원형 배지 — 누가 로그인해 있는지 한눈에 보이게
    userAvatarLabel = new QLabel();
    userAvatarLabel->setObjectName("userAvatar");
    userAvatarLabel->setFixedSize(28, 28);
    userAvatarLabel->setAlignment(Qt::AlignCenter);
    userAvatarLabel->setText(currentUser.name.left(1));
    lay->addWidget(userAvatarLabel);

    userNameLabel = new QLabel();
    userNameLabel->setObjectName("userName");
    userNameLabel->setText(currentUser.name);
    userNameLabel->setToolTip(QStringLiteral("%1 (%2)")
                                  .arg(currentUser.name, currentUser.loginId));
    lay->addWidget(userNameLabel);

    logoutButton = new QPushButton(QStringLiteral("로그아웃"));
    logoutButton->setObjectName("logoutButton");
    logoutButton->setCursor(Qt::PointingHandCursor);
    connect(logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    lay->addWidget(logoutButton);

    return header;
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
    close();
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

    // ── ROI 도구: 성격이 같은 3형제를 하나의 세그먼트 그룹으로 묶는다 ──
    auto* roiGroup = new QFrame();
    roiGroup->setObjectName("segGroup");
    auto* roiGroupLay = new QHBoxLayout(roiGroup);
    roiGroupLay->setContentsMargins(9, 3, 3, 3);
    roiGroupLay->setSpacing(2);

    auto* roiCaption = new QLabel(QStringLiteral("ROI"));
    roiCaption->setObjectName("segCaption");
    roiGroupLay->addWidget(roiCaption);

    roiButton = new QPushButton(QStringLiteral("지정"));
    roiButton->setObjectName("segBtn");
    roiButton->setCursor(Qt::PointingHandCursor);
    connect(roiButton, &QPushButton::clicked, this, &MainWindow::onRoiButtonClicked);
    roiGroupLay->addWidget(roiButton);

    roiClearButton = new QPushButton(QStringLiteral("제거"));
    roiClearButton->setObjectName("segBtnDanger");
    roiClearButton->setCursor(Qt::PointingHandCursor);
    connect(roiClearButton, &QPushButton::clicked, this, &MainWindow::onRoiClearClicked);
    roiGroupLay->addWidget(roiClearButton);

    roiToggleButton = new QPushButton(QStringLiteral("표시"));
    roiToggleButton->setObjectName("segBtnToggle");
    roiToggleButton->setCheckable(true);
    roiToggleButton->setChecked(true);
    roiToggleButton->setCursor(Qt::PointingHandCursor);
    connect(roiToggleButton, &QPushButton::toggled, this,
            &MainWindow::onRoiVisibilityToggled);
    roiGroupLay->addWidget(roiToggleButton);

    titleRow->addWidget(roiGroup);

    // ROI 도구와 실시간 액션 사이 구분선
    auto* toolSep = new QFrame();
    toolSep->setFrameShape(QFrame::VLine);
    toolSep->setObjectName("toolSep");
    toolSep->setFixedHeight(22);
    titleRow->addWidget(toolSep);

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

    // 📷 카메라 연결 — CCTV IP를 입력받아 서버로 전송(서버가 그 IP로 RTSP를 연다)
    addCameraButton = new QPushButton(QStringLiteral("📷 카메라 연결"));
    addCameraButton->setObjectName("roiButton");
    addCameraButton->setCursor(Qt::PointingHandCursor);
    connect(addCameraButton, &QPushButton::clicked, this,
            &MainWindow::onAddCameraClicked);
    titleRow->addWidget(addCameraButton);

    // 🔍 카메라 검색 — 같은 망의 ONVIF 카메라를 자동 탐색해 목록에서 선택
    searchCameraButton = new QPushButton(QStringLiteral("🔍 카메라 검색"));
    searchCameraButton->setObjectName("roiButton");
    searchCameraButton->setCursor(Qt::PointingHandCursor);
    connect(searchCameraButton, &QPushButton::clicked, this,
            &MainWindow::onSearchCameraClicked);
    titleRow->addWidget(searchCameraButton);

    // 카메라 해제 — 모든 채널 연결 끊기(서버가 RTSP 종료 후 대기)
    clearCameraButton = new QPushButton(QStringLiteral("카메라 해제"));
    clearCameraButton->setObjectName("segBtnDanger");
    clearCameraButton->setCursor(Qt::PointingHandCursor);
    connect(clearCameraButton, &QPushButton::clicked, this,
            &MainWindow::onCameraClearClicked);
    titleRow->addWidget(clearCameraButton);

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
    card->setMinimumSize(420, 280);

    auto* lay = new QVBoxLayout(card);
    card->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // 상단 오버레이 바: 병상/환자 + LIVE
    auto* bar = new QFrame();
    bar->setObjectName("videoBar");
    bar->setFixedHeight(27);
    auto* barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(8, 0, 8, 0);
    barLay->setSpacing(6);

    auto* bed = new QLabel(patients[channel].bed);
    bed->setObjectName("bedBadge");
    auto* name = new QLabel(patients[channel].name);
    name->setObjectName("bedName");
    barLay->addWidget(bed);
    barLay->addWidget(name);
    barLay->addStretch();

    auto* livePill = new QFrame();
    livePill->setObjectName("livePill");
    auto* lpLay = new QHBoxLayout(livePill);
    lpLay->setContentsMargins(7, 1, 8, 1);
    lpLay->setSpacing(5);
    liveDots[channel] = new QLabel();
    liveDots[channel]->setObjectName("liveDotOff");
    liveDots[channel]->setFixedSize(6, 6);
    auto* liveTxt = new QLabel(QStringLiteral("LIVE"));
    liveTxt->setObjectName("liveText");
    lpLay->addWidget(liveDots[channel]);
    lpLay->addWidget(liveTxt);
    barLay->addWidget(livePill);

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
                    roiButton->setText(on ? QStringLiteral("취소")
                                          : QStringLiteral("지정"));
            });
    lay->addWidget(video, 1);

    return card;
}

QWidget* MainWindow::buildVitalsPanel()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");
    // 바이탈 패널은 폭 고정 → 창을 키우면 남는 폭이 전부 영상 월로 간다.
    panel->setMinimumWidth(300);
    panel->setMaximumWidth(340);

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

    // 헤더: 상태등 + 이름 + 병상 + 상태 배지
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    vitalStatusDots[channel] = new QLabel();
    vitalStatusDots[channel]->setObjectName("vitalDot");
    vitalStatusDots[channel]->setFixedSize(9, 9);
    auto* name = new QLabel(patients[channel].name);
    name->setObjectName("vitalName");
    auto* bed = new QLabel(patients[channel].bed);
    bed->setObjectName("vitalBed");
    vitalStatusBadges[channel] = new QLabel(QStringLiteral("대기"));
    vitalStatusBadges[channel]->setObjectName("vitalBadge");
    vitalStatusBadges[channel]->setAlignment(Qt::AlignCenter);
    head->addWidget(vitalStatusDots[channel]);
    head->addWidget(name);
    head->addWidget(bed);
    head->addStretch();
    head->addWidget(vitalStatusBadges[channel]);
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
    body->addWidget(buildCareTimeDashboard(), 4);

    outer->addLayout(body, 1);

    // 블랙박스 플레이어는 인라인 대신 팝업 다이얼로그로 크게 재생한다.
    // (로그 더블클릭 → onLogRowActivated에서 다이얼로그를 띄우고 재생)
    buildBlackboxDialog();
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
        QStringLiteral("블랙박스 영상을 불러오는 중…"));
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

void MainWindow::buildBlackboxDialog()
{
    blackboxDialog = new QDialog(this);
    blackboxDialog->setObjectName("blackboxDialog");
    blackboxDialog->setWindowTitle(QStringLiteral("블랙박스 영상 재생"));
    blackboxDialog->resize(960, 640);
    blackboxDialog->setMinimumSize(560, 420);

    auto* dl = new QVBoxLayout(blackboxDialog);
    dl->setContentsMargins(14, 14, 14, 14);
    dl->addWidget(buildBlackboxPlayer());  // 기존 플레이어 카드/컨트롤 그대로 재사용

    // 팝업을 닫으면 재생을 멈춰 리소스를 정리한다.
    connect(blackboxDialog, &QDialog::finished, this, [this](int) {
        if (blackboxPlayer) blackboxPlayer->stop();
    });
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
        #clock { color: %(text); font-size: 15px; font-weight: 700; letter-spacing: 0.5px; }
        #headerSep { color: %(border); }

        /* 라이트/다크 테마 토글 */
        #themeToggle { background: %(card); border: 1px solid %(border); border-radius: 8px;
                       padding: 3px 10px; font-size: 14px; }
        #themeToggle:hover { border-color: %(accent); }

        /* 연결 상태 pill 배지 */
        #statusPill { background: %(card); border: 1px solid %(border); border-radius: 12px; }
        #statusText { color: %(sub); font-size: 12px; font-weight: 600; }

        /* 로그인 사용자 표시 + 로그아웃 */
        #userAvatar { background: %(accent); color: #fff; border-radius: 14px;
                      font-size: 13px; font-weight: 800; }
        #userName { color: %(text); font-size: 13px; font-weight: 700; }
        #logoutButton { background: %(card); color: %(sub); border: 1px solid %(border);
                        border-radius: 8px; padding: 5px 12px; font-size: 12px; font-weight: 600; }
        #logoutButton:hover { border-color: %(critical); color: %(critical); }

        #panel { background: %(panel); border: 1px solid %(border); border-radius: 12px; }
        /* 섹션 제목: 좌측 청록 악센트 바로 위계 부여 */
        #panelTitle { color: %(text); font-size: 15px; font-weight: 800;
                      border-left: 3px solid %(accent); padding-left: 10px; }

        /* 블랙박스 재생 팝업 */
        #blackboxDialog { background: %(bgDeep); }

        #roiButton, #roiToggle, #roiClear { background: %(card); color: %(text); border: 1px solid %(border);
                                 border-radius: 8px; padding: 6px 14px; font-size: 12px; font-weight: 600; }
        #roiButton:hover, #roiToggle:hover { border-color: %(accent); }
        #roiToggle:checked { background: %(accent); color: #fff; border-color: %(accent); }
        #roiClear:hover { border-color: %(critical); color: %(critical); }

        /* ── 관제화면 ROI 세그먼트 그룹 ── */
        #segGroup { background: %(bgDeep); border: 1px solid %(border); border-radius: 9px; }
        #segCaption { color: %(sub); font-size: 11px; font-weight: 800; letter-spacing: 1px; }
        #segBtn, #segBtnDanger, #segBtnToggle {
            background: transparent; color: %(text); border: none;
            border-radius: 6px; padding: 5px 13px; font-size: 12px; font-weight: 600; }
        #segBtn:hover, #segBtnToggle:hover { background: %(card); }
        #segBtnDanger:hover { background: %(card); color: %(critical); }
        #segBtnToggle:checked { background: %(accent); color: #fff; }
        #toolSep { color: %(border); }

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
        #bedBadge { background: %(accent); color: #fff; font-size: 10px; font-weight: 800;
                    padding: 1px 6px; border-radius: 5px; letter-spacing: 0.5px; }
        #bedName { color: %(text); font-size: 12px; font-weight: 600; }
        /* LIVE pill 배지 */
        #livePill { background: %(card); border: 1px solid %(border); border-radius: 9px; }
        #liveText { color: %(sub); font-size: 9px; font-weight: 800; letter-spacing: 1.5px; }
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
        QTabBar { qproperty-drawBase: 0; }
        QTabBar::tab { background: transparent; color: %(sub); padding: 10px 20px;
                       border: none; border-bottom: 2px solid transparent;
                       font-size: 13px; font-weight: 700; margin-right: 4px; }
        QTabBar::tab:selected { color: %(accent); border-bottom: 2px solid %(accent); }
        QTabBar::tab:hover:!selected { color: %(text); }

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
    statusDot->setStyleSheet(QString("background:%1; border-radius:3px;").arg(kCritical));
    for (int i = 0; i < 4; ++i) {
        liveDots[i]->setStyleSheet(QString("background:%1; border-radius:3px;").arg(kTextSub));
        vitalStatusDots[i]->setStyleSheet(QString("background:%1; border-radius:5px;").arg(kTextSub));
    }
}

void MainWindow::toggleTheme()
{
    darkMode = !darkMode;
    applyPalette(darkMode ? kDark : kLight);
    applyTheme();  // 바뀐 팔레트로 QSS 재생성·재적용

    if (themeToggleButton)
        themeToggleButton->setText(darkMode ? QStringLiteral("☀")
                                            : QStringLiteral("🌙"));

    // applyTheme가 상태등을 기본값(빨강/회색)으로 리셋하므로 현재 상태를 즉시 복원한다.
    bool connected = true;
    for (int i = 0; i < kNumServers; ++i)
        if (sockets[i]->state() != QAbstractSocket::ConnectedState) connected = false;
    setConnectionState(connected, statusText->text());
    updateVitals();  // 바이탈 색/배지를 새 팔레트 기준으로 즉시 갱신
}

void MainWindow::setConnectionState(bool connected, const QString& text)
{
    if (!statusDot) return;
    const char* color = connected ? kNormal : kCritical;
    statusDot->setStyleSheet(QString("background:%1; border-radius:3px;").arg(color));
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
    // 끊겨 있는 서버 소켓만 골라 재접속 (일부 Pi만 끊긴 경우도 처리)
    for (int i = 0; i < kNumServers; ++i) {
        if (sockets[i]->state() != QAbstractSocket::UnconnectedState) continue;
        buffers[i].clear();  // 이전 연결의 파싱 잔여물 폐기
        const QString host = serverHost(i);
        sockets[i]->connectToHost(QHostAddress(host), kServerPort);
        qDebug() << "영상 서버 접속 시도:" << host << ":" << kServerPort;
    }
}

void MainWindow::onSocketStateChanged(QAbstractSocket::SocketState /*state*/)
{
    // 소켓이 여러 개(2-Pi)라 개별 state가 아니라 "전체 집계"로 상태 표시를 갱신한다.
    int connected = 0, unconnected = 0;
    for (int i = 0; i < kNumServers; ++i) {
        const auto st = sockets[i]->state();
        if (st == QAbstractSocket::ConnectedState) ++connected;
        else if (st == QAbstractSocket::UnconnectedState) ++unconnected;
    }

    if (connected == kNumServers)
        setConnectionState(true, QStringLiteral("영상 서버 연결됨"));
    else if (connected > 0)
        setConnectionState(false, QStringLiteral("영상 서버 %1/%2 연결")
                                      .arg(connected).arg(kNumServers));
    else
        setConnectionState(false, QStringLiteral("영상 서버 접속 중..."));

    // 담당 Pi가 끊긴 채널의 LIVE 표시등 소등
    for (int ch = 0; ch < 4; ++ch) {
        if (sockets[serverForChannel(ch)]->state() != QAbstractSocket::ConnectedState)
            liveDots[ch]->setStyleSheet(QString("background:%1; border-radius:3px;").arg(kTextSub));
    }

    // Pi가 새로 연결됨(false→true)을 감지 → 저장된 카메라를 자동 재전송.
    // (서버는 재접속 후 카메라를 모르므로, 사용자가 다시 누르지 않아도 복구된다.)
    for (int i = 0; i < kNumServers; ++i) {
        const bool now = sockets[i]->state() == QAbstractSocket::ConnectedState;
        if (now && !serverConnected_[i])
            resendCamerasForServer(i);
        serverConnected_[i] = now;
    }

    // 끊긴 소켓이 하나라도 있으면 재접속 예약
    if (unconnected > 0 && !reconnectTimer.isActive())
        reconnectTimer.start();
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

        vitalStatusDots[i]->setStyleSheet(QString("background:%1; border-radius:4px;").arg(color));

        const QString status = vitalStatusLabel(temp, hr);
        vitalStatusBadges[i]->setText(status);
        vitalStatusBadges[i]->setStyleSheet(QString(
            "color:%1; background:%2; border:1px solid %1; border-radius:9px;"
            " padding:1px 10px; font-size:11px; font-weight:800;")
            .arg(color, blendHex(color, kCard, 0.18)));

        vitalUpdated[i]->setText(QStringLiteral("웨어러블 · 마지막 갱신 ") + now);
    }
}

// ═══════════════════════════════════════════════════════════
//  영상 수신 (명세서 프로토콜 파싱)
// ═══════════════════════════════════════════════════════════
void MainWindow::onReadyRead()
{
    // 어느 서버 소켓(Pi)이 신호를 냈는지 판별 → 그 소켓 전용 버퍼로 파싱한다.
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    int idx = 0;
    for (int i = 0; i < kNumServers; ++i)
        if (sock == sockets[i]) { idx = i; break; }
    QByteArray& buffer = buffers[idx];  // 이하 기존 파싱 코드는 이 지역 참조를 그대로 사용

    // 🌟 명세서 가이드: 들어온 데이터를 무조건 (이 소켓의) 버퍼 뒤에 붙임
    buffer.append(sock->readAll());

    // 🌟 지연(백로그) 제거: 이번 호출에 도착한 프레임을 채널별로 훑어
    //    "가장 최신 1장"만 남긴다. 그 사이 오래된 프레임은 디코드·렌더를
    //    아예 건너뛴다(화면엔 어차피 최신 것만 보이므로 헛일). 이렇게 하면
    //    소비자(GUI 스레드)가 유입 속도를 못 따라가도 backlog가 쌓이지 않는다.
    //    ※ 이벤트(낙상 등)는 절대 스킵하지 않고 파싱 즉시 처리한다.
    QByteArray latestJpeg[4];               // 채널별 최신 프레임 JPEG 바이트
    quint64    latestTs[4]  = {0, 0, 0, 0}; // 채널별 최신 프레임 서버 타임스탬프
    bool       hasFrame[4]  = {false, false, false, false};

    // 버퍼에 완성된 패킷이 남아있는 동안 반복 파싱
    while (true) {
        // 0) 매직(2바이트)으로 패킷 종류 식별 — 영상(0xDB4B) / 이벤트(0xDB4D)
        if (buffer.size() < (int)sizeof(uint16_t))
            break;  // 데이터 더 올 때까지 대기 → 아래에서 최신 프레임 렌더
        uint16_t magic;
        memcpy(&magic, buffer.constData(), sizeof(magic));

        // ── 이벤트 패킷 (낙상 통보 등, 페이로드 없음) — 스킵 금지, 즉시 처리 ──
        if (magic == kEvtMagic) {
            if (buffer.size() < (int)sizeof(dbj_evt_header_t))
                break;  // 헤더가 덜 옴 — 다음 readyRead 대기
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
            buffer.clear(); // 오염된 버퍼 초기화
            sock->abort(); // 즉시 끊기 → UnconnectedState → 재접속 타이머 가동
            return;
        }

        // 4) 전체 패킷 크기 계산 = 헤더(16B) + 진짜 JPEG 크기
        //    (qint64 — uint32 payload_len과의 int 오버플로우 방지)
        const qint64 total = static_cast<qint64>(sizeof(header)) + header.payload_len;

        // JPEG 데이터가 아직 다 안 왔으면 다음 readyRead 때까지 대기
        if (buffer.size() < total)
            break;

        // 5) 디코드는 뒤로 미루고, 채널별 "최신 프레임" 바이트만 보관한다.
        //    (여기서 QImage 디코드/렌더를 하면 backlog의 오래된 프레임까지
        //     전부 처리하게 되어 지연이 톱니처럼 쌓인다.)
        if (header.channel < 4) {
            latestJpeg[header.channel] = QByteArray(
                buffer.constData() + sizeof(header),
                static_cast<int>(header.payload_len)); // 이전 최신 프레임 덮어씀
            latestTs[header.channel]  = header.timestamp_ms;
            hasFrame[header.channel]  = true;
        }

        // 6) 사용이 끝난 패킷만큼 버퍼 맨 앞에서 도려내기
        buffer.remove(0, static_cast<int>(total));
    }

    // 7) 채널별 "가장 최신" 프레임만 디코드·렌더 (헛일 제거로 지연 최소화)
    for (int ch = 0; ch < 4; ++ch) {
        if (!hasFrame[ch])
            continue;

        QImage image = QImage::fromData(
            reinterpret_cast<const uchar*>(latestJpeg[ch].constData()),
            latestJpeg[ch].size(),
            "JPEG");
        if (image.isNull())
            continue;

        // 지연 시간(Latency) 모니터링 — 최신 프레임 기준으로만 출력
        // ※ 서버·클라 PC 시계가 NTP로 동기화돼 있어야 값이 정확하다.
        //   (동기 안 되면 두 시계 오프셋만큼 음수/양수로 치우침)
        const qint64 latency =
            QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(latestTs[ch]);
        //qDebug() << "Channel:" << ch << " | Latency:" << latency << "ms";

        channelViews[ch]->setFrame(QPixmap::fromImage(image));
        liveDots[ch]->setStyleSheet(
            QString("background:%1; border-radius:3px;").arg(kCritical));
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
            channelViews[channel]->setAlert(true, QStringLiteral("🚨 낙상 감지"));
        }
        qDebug() << "🚨 [낙상 감지] 채널" << (channel + 1) << "빨간 테두리 켜짐 (모자이크 자동 해제 상태)";
    }

    // 2. 비상 로그 조회 탭에 URL 및 정보 등록
    if (logTable) {
        logTable->setSortingEnabled(false);   // 삽입 중 재정렬 방지

        const int row = logTable->rowCount();
        logTable->insertRow(row);
        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 static_cast<qint64>(timestampMs)).toString("yyyy-MM-dd HH:mm:ss");
        auto* dtItem = new QTableWidgetItem(when);
        // 서버 저장 규칙: 낙상은 _FALL 접미사 — chN_타임스탬프_FALL.mp4
        const QString clipUrl = QStringLiteral("http://%1:%2/ch%3_%4_FALL.mp4")
                                     .arg(hostForChannel(channel))
                                     .arg(kClipHttpPort)
                                     .arg(channel)
                                     .arg(timestampMs);
        dtItem->setData(Qt::UserRole, clipUrl);
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("낙상")));
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));

        logTable->setSortingEnabled(true);
        logTable->sortItems(0, Qt::DescendingOrder);   // 최신 이벤트가 위로
    }
}

// ═══════════════════════════════════════════════════════════
//  침상 이탈 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleBedEgressEvent(int channel, quint64 timestampMs)
{
    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        bedEgressActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(true, QStringLiteral("⚠️ 침대 이탈"));
        }
        qDebug() << "⚠️ [침상 이탈 감지] 채널" << (channel + 1) << "빨간 테두리 켜짐";
    }

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable) {
        logTable->setSortingEnabled(false);   // 삽입 중 재정렬 방지

        const int row = logTable->rowCount();
        logTable->insertRow(row);

        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 static_cast<qint64>(timestampMs)).toString("yyyy-MM-dd HH:mm:ss");
        auto* dtItem = new QTableWidgetItem(when);

        const QString clipUrl = QStringLiteral("http://%1:%2/ch%3_%4_EGRESS.mp4")
                                     .arg(hostForChannel(channel))
                                     .arg(kClipHttpPort)
                                     .arg(channel)
                                     .arg(timestampMs);
        dtItem->setData(Qt::UserRole, clipUrl);
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("침상 이탈"))); // 💡 이탈 분류로 등록
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));

        logTable->setSortingEnabled(true);
        logTable->sortItems(0, Qt::DescendingOrder);   // 최신 이벤트가 위로
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
        roiToggleButton->setText(on ? QStringLiteral("표시")
                                    : QStringLiteral("숨김"));
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
    QTcpSocket* sock = socketForChannel(channel);   // 이 채널을 담당하는 Pi 소켓
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
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
    sock->write(pkt);
    sock->flush();
}

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

// Scopes 문자열에서 모델명 추출 (name 우선, hardware 보조).
QString modelFromScopes(const QString& scopes) {
    QString name, hardware;
    const QStringList toks =
        scopes.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const QString& t : toks) {
        int i;
        if ((i = t.indexOf(QStringLiteral("/name/"))) >= 0)
            name = QUrl::fromPercentEncoding(t.mid(i + 6).toUtf8());
        else if ((i = t.indexOf(QStringLiteral("/hardware/"))) >= 0)
            hardware = QUrl::fromPercentEncoding(t.mid(i + 10).toUtf8());
    }
    if (!name.isEmpty() && !hardware.isEmpty())
        return name + QStringLiteral(" (") + hardware + QStringLiteral(")");
    if (!name.isEmpty()) return name;
    if (!hardware.isEmpty()) return hardware;
    return QStringLiteral("(알 수 없음)");
}

// UUID 꼬리(마지막 12 hex)를 MAC 형식으로 — 다수 카메라가 UUID에 MAC을 심는다.
QString macFromUuid(const QString& uuid) {
    QString hex;
    for (QChar c : uuid) if (c.isLetterOrNumber()) hex += c;
    if (hex.size() < 12) return QString();
    const QString tail = hex.right(12).toUpper();
    for (QChar c : tail)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return QString();
    QStringList parts;
    for (int i = 0; i < 12; i += 2) parts << tail.mid(i, 2);
    return parts.join(':');
}

// ProbeMatch 응답 1개 파싱 → DiscoveredCam.
DiscoveredCam parseProbeMatch(const QByteArray& datagram) {
    DiscoveredCam cam;
    QString xaddrs, scopes;
    QXmlStreamReader xml(datagram);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();  // 네임스페이스 접두어 제외 로컬명
            if (name == QStringLiteral("XAddrs"))       xaddrs = xml.readElementText();
            else if (name == QStringLiteral("Scopes"))  scopes = xml.readElementText();
            else if (name == QStringLiteral("Address") && cam.uuid.isEmpty())
                cam.uuid = xml.readElementText().trimmed();
        }
    }
    if (!xaddrs.isEmpty()) {
        const QString first =
            xaddrs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).value(0);
        cam.ip = QUrl(first).host();
    }
    cam.model = modelFromScopes(scopes);
    cam.mac = macFromUuid(cam.uuid);
    if (cam.mac.isEmpty()) cam.mac = cam.uuid;
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
    QTcpSocket* sock = socketForChannel(channel);   // 이 채널 담당 Pi 소켓
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        return false;   // 연결 안 됨 — 호출자가 모아서 안내
    }

    const QByteArray url = rtspUrl.toUtf8();
    const int len = qMin(url.size(), kCameraUrlMax);

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlCameraSet;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = static_cast<uint16_t>(len);   // 이어지는 URL 바이트 길이

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    pkt.append(url.constData(), len);
    sock->write(pkt);
    sock->flush();
    return true;
}

void MainWindow::sendCameraClear(int channel)
{
    QTcpSocket* sock = socketForChannel(channel);
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlCameraClear;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = 0;

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    sock->write(pkt);
    sock->flush();
}

void MainWindow::onAddCameraClicked()
{
    // 마지막 입력값을 QSettings에서 복원 (관제 PC 로컬).
    QSettings s;
    const QString lastIp      = s.value(QStringLiteral("camera/ip")).toString();
    const QString lastUser    = s.value(QStringLiteral("camera/user"),
                                        QStringLiteral("admin")).toString();
    const int     lastPort    = s.value(QStringLiteral("camera/port"), 554).toInt();
    const QString lastProfile = s.value(QStringLiteral("camera/profile"),
                                        QStringLiteral("profile2")).toString();

    // ── 입력 다이얼로그 ──────────────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("카메라 연결"));
    auto* form = new QFormLayout(&dlg);

    auto* ipEdit = new QLineEdit(lastIp, &dlg);
    ipEdit->setPlaceholderText(QStringLiteral("예: 172.20.35.140"));
    auto* userEdit = new QLineEdit(lastUser, &dlg);
    auto* pwEdit = new QLineEdit(&dlg);
    pwEdit->setEchoMode(QLineEdit::Password);
    pwEdit->setPlaceholderText(QStringLiteral("CCTV 비밀번호"));
    auto* portEdit = new QLineEdit(QString::number(lastPort), &dlg);
    auto* profileEdit = new QLineEdit(lastProfile, &dlg);

    form->addRow(QStringLiteral("CCTV IP"), ipEdit);
    form->addRow(QStringLiteral("계정"), userEdit);
    form->addRow(QStringLiteral("비밀번호"), pwEdit);
    form->addRow(QStringLiteral("포트"), portEdit);
    form->addRow(QStringLiteral("프로파일"), profileEdit);

    auto* hint = new QLabel(
        QStringLiteral("IP 하나로 4채널(센서)을 모두 연결합니다.\n"
                       "ch0·1은 Pi A, ch2·3은 Pi B로 전송됩니다."), &dlg);
    hint->setObjectName("segCaption");
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("연결"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("취소"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QString ip = ipEdit->text().trimmed();
    if (ip.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("CCTV IP를 입력하세요."));
        return;
    }
    const QString user = userEdit->text().trimmed();
    const QString pw = pwEdit->text();
    bool portOk = false;
    const int port = portEdit->text().trimmed().toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("포트 번호가 올바르지 않습니다."));
        return;
    }
    const QString profile = profileEdit->text().trimmed().isEmpty()
                                ? QStringLiteral("profile2")
                                : profileEdit->text().trimmed();

    connectCameraWith(ip, user, pw, port, profile);
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

    // IP 하나 → 4채널 URL 생성 → 채널별 담당 Pi로 전송(socketForChannel이 라우팅).
    int sent = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const QString url = buildRtspUrl(ip, user, password, port, prof, ch);
        lastCameraUrl_[ch] = url;   // 재접속 시 자동 재전송용(세션 한정)
        if (channelViews[ch])
            channelViews[ch]->setCameraConnected(true);  // "신호 대기 중…" 표시
        if (sendCamera(ch, url)) ++sent;
    }

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

void MainWindow::onSearchCameraClicked()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("카메라 검색 (ONVIF)"));
    dlg.resize(560, 440);
    auto* v = new QVBoxLayout(&dlg);

    auto* status = new QLabel(QStringLiteral("같은 망의 ONVIF 카메라를 검색 중…"), &dlg);
    v->addWidget(status);

    auto* table = new QTableWidget(0, 3, &dlg);
    table->setHorizontalHeaderLabels({QStringLiteral("모델"), QStringLiteral("IP"),
                                      QStringLiteral("MAC / ID")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    v->addWidget(table, 1);

    // 접속에 필요한 계정/포트/프로파일 (마지막 값 복원)
    QSettings s;
    auto* form = new QFormLayout();
    auto* userEdit = new QLineEdit(
        s.value(QStringLiteral("camera/user"), QStringLiteral("admin")).toString(), &dlg);
    auto* pwEdit = new QLineEdit(&dlg);
    pwEdit->setEchoMode(QLineEdit::Password);
    pwEdit->setPlaceholderText(QStringLiteral("CCTV 비밀번호"));
    auto* portEdit = new QLineEdit(
        QString::number(s.value(QStringLiteral("camera/port"), 554).toInt()), &dlg);
    auto* profileEdit = new QLineEdit(
        s.value(QStringLiteral("camera/profile"), QStringLiteral("profile2")).toString(), &dlg);
    form->addRow(QStringLiteral("계정"), userEdit);
    form->addRow(QStringLiteral("비밀번호"), pwEdit);
    form->addRow(QStringLiteral("포트"), portEdit);
    form->addRow(QStringLiteral("프로파일"), profileEdit);
    v->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("이 카메라로 연결"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("닫기"));
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(table, &QTableWidget::cellDoubleClicked, &dlg,
            [&dlg](int, int) { dlg.accept(); });  // 더블클릭으로도 연결

    // ── WS-Discovery: 멀티캐스트 Probe 전송 → 유니캐스트 ProbeMatch 수신 ──
    auto* udp = new QUdpSocket(&dlg);
    if (!udp->bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::ShareAddress))
        status->setText(QStringLiteral("검색 소켓 열기 실패 — 방화벽/권한 확인"));

    QSet<QString> seen;   // dlg.exec()가 블로킹하는 동안 이 지역변수는 살아 있음
    connect(udp, &QUdpSocket::readyRead, &dlg, [udp, table, &seen]() {
        while (udp->hasPendingDatagrams()) {
            QByteArray dg;
            dg.resize(static_cast<int>(udp->pendingDatagramSize()));
            QHostAddress from;
            udp->readDatagram(dg.data(), dg.size(), &from);
            const DiscoveredCam cam = parseProbeMatch(dg);
            qDebug() << "WS-Discovery 응답" << from.toString()
                     << "→ ip:" << cam.ip << "model:" << cam.model;
            if (cam.ip.isEmpty()) continue;
            const QString key = cam.uuid.isEmpty() ? cam.ip : cam.uuid;
            if (seen.contains(key)) continue;   // 재전송/중복 응답 제거
            seen.insert(key);
            const int r = table->rowCount();
            table->insertRow(r);
            table->setItem(r, 0, new QTableWidgetItem(cam.model));
            table->setItem(r, 1, new QTableWidgetItem(cam.ip));
            table->setItem(r, 2, new QTableWidgetItem(cam.mac));
        }
    });

    const QByteArray probe = buildWsDiscoveryProbe();
    const QHostAddress mcast(QStringLiteral("239.255.255.250"));

    // ★ 핵심: 기본 멀티캐스트 인터페이스가 가상 어댑터로 잡히면 카메라가 Probe를
    //   못 받는다 → IPv4·멀티캐스트 가능한 모든 인터페이스로 각각 쏜다.
    auto sendProbes = [udp, probe, mcast]() {
        int sentOn = 0;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            const auto f = iface.flags();
            if (!f.testFlag(QNetworkInterface::IsUp) ||
                !f.testFlag(QNetworkInterface::IsRunning) ||
                f.testFlag(QNetworkInterface::IsLoopBack) ||
                !f.testFlag(QNetworkInterface::CanMulticast))
                continue;
            bool hasV4 = false;
            for (const auto& e : iface.addressEntries())
                if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) { hasV4 = true; break; }
            if (!hasV4) continue;
            udp->setMulticastInterface(iface);
            const qint64 n = udp->writeDatagram(probe, mcast, 3702);
            qDebug() << "WS-Discovery Probe →" << iface.humanReadableName()
                     << "bytes:" << n;
            if (n > 0) ++sentOn;
        }
        if (sentOn == 0)  // 폴백: 기본 인터페이스로라도 전송
            udp->writeDatagram(probe, mcast, 3702);
    };

    sendProbes();
    QTimer::singleShot(1200, &dlg, sendProbes);   // UDP 유실 대비 재전송
    QTimer::singleShot(5000, &dlg, [status, table]() {
        status->setText(QStringLiteral("검색 완료 — %1대 발견 (없으면 '카메라 연결'로 IP 직접 입력)")
                            .arg(table->rowCount()));
    });

    if (dlg.exec() != QDialog::Accepted) return;

    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 1)) {
        QMessageBox::information(this, QStringLiteral("카메라 검색"),
                                 QStringLiteral("목록에서 카메라를 선택하세요."));
        return;
    }
    bool portOk = false;
    const int port = portEdit->text().trimmed().toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("포트 번호가 올바르지 않습니다."));
        return;
    }
    connectCameraWith(table->item(row, 1)->text(), userEdit->text().trimmed(),
                      pwEdit->text(), port, profileEdit->text().trimmed());
}

void MainWindow::onCameraClearClicked()
{
    bool any = false;
    for (int ch = 0; ch < 4; ++ch)
        if (!lastCameraUrl_[ch].isEmpty()) any = true;
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
        if (channelViews[ch])
            channelViews[ch]->setCameraConnected(false);  // "카메라 미연결" 표시로 복귀
    }
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
            if (channelViews[ch]) channelViews[ch]->setCameraConnected(true);
            qDebug() << "Pi" << serverIdx << "재접속 → ch" << ch << "카메라 자동 재전송";
        }
    }
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
        if (fallActive[channel] || bedEgressActive[channel]) {
            // 1. 빨간 테두리 끄고 로컬 경보 상태 클리어 (낙상·침상이탈 모두)
            fallActive[channel] = false;
            bedEgressActive[channel] = false;
            if (channelViews[channel]) {
                channelViews[channel]->setAlert(false);
            }

            // 2. 서버의 PrivacyMasker를 깨워서 다시 모자이크 씌우라고 0x03 바이너리 쏘기
            //    (채널마다 담당 Pi가 다르므로 그 채널의 소켓으로 보낸다)
            QTcpSocket* sock = socketForChannel(channel);
            if (sock && sock->state() == QAbstractSocket::ConnectedState) {
                dbj_ctrl_header_t h;
                h.magic = kCtrlMagic;                  // 0xDB4C
                h.version = 0x01;
                h.type = kCtrlAlarmConfirm;             // 0x03 (경보 확인 -> 마스크 복구)
                h.channel = static_cast<uint8_t>(channel);
                h.point_count = 0;
                h.reserved = 0;

                sock->write(reinterpret_cast<const char*>(&h), sizeof(h));
                sock->flush();  // 채널별 소켓이 다를 수 있어 즉시 flush
                packetSent = true;
                qDebug() << "🔓 [Qt -> 서버] 채널" << channel << "경보 확인 및 모자이크 복구 패킷 전송!";
            }
        }
    }

    // (flush는 위 루프에서 채널별 소켓마다 이미 처리)
    if (!packetSent) {
        // 현재 활성화된 경보가 아예 없을 때만 안내 메시지 표시
        QMessageBox::information(this, QStringLiteral("경보 해제"),
                                 QStringLiteral("현재 활성화된 낙상/침상이탈 경보가 없습니다."));
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
    if (blackboxDialog) {
        blackboxDialog->show();
        blackboxDialog->raise();
        blackboxDialog->activateWindow();
    }
    playBlackboxClip(url);
}

// ═══════════════════════════════════════════════════════════
//  TAB3 슬롯 — 자리표시자 (DB 쿼리 연동 전)
// ═══════════════════════════════════════════════════════════
void MainWindow::refreshResidentTable()
{
    if (!residentTable) return;
    residentTable->setRowCount(0);

    // main.cpp에서 열어둔 기본 연결(QMARIADB) 사용
    QSqlQuery q(QStringLiteral(
        "SELECT resident_id, name, room, bed, camera_id, wearable_id, "
        "risk_level, status FROM residents ORDER BY resident_id"));
    if (q.lastError().isValid()) {
        qDebug() << "입소자 목록 조회 실패:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        const int row = residentTable->rowCount();
        residentTable->insertRow(row);
        for (int col = 0; col < 8; ++col) {
            const QVariant v = q.value(col);
            auto* item = new QTableWidgetItem(
                v.isNull() ? QStringLiteral("-") : v.toString());
            residentTable->setItem(row, col, item);
        }
    }
    qDebug() << "입소자 목록 새로고침 —" << residentTable->rowCount() << "명 로드";
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
    if (!idItem) return;
    const int id = idItem->text().toInt();

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT name, room, bed, camera_id, wearable_id, risk_level, "
        "admitted_at, discharge_due, status, guardian_name, guardian_phone, "
        "guardian_relation, notes FROM residents WHERE resident_id = ?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) {
        qDebug() << "입소자 상세 조회 실패:" << q.lastError().text();
        return;
    }

    selectedResidentId = id;

    // 콤보는 텍스트로 매칭해 선택
    auto setCombo = [](QComboBox* c, const QString& text) {
        const int i = c->findText(text);
        if (i >= 0) c->setCurrentIndex(i);
    };

    editName->setText(q.value(0).toString());
    editRoom->setText(q.value(1).toString());
    editBed->setText(q.value(2).toString());
    editCameraId->setText(q.value(3).isNull() ? QString() : q.value(3).toString());
    editWearableId->setText(q.value(4).isNull() ? QString() : q.value(4).toString());
    setCombo(editRiskLevel, q.value(5).toString());
    if (q.value(6).toDate().isValid())  editAdmittedAt->setDate(q.value(6).toDate());
    if (q.value(7).toDate().isValid())  editDischargeDue->setDate(q.value(7).toDate());
    setCombo(editStatus, q.value(8).toString());
    editGuardianName->setText(q.value(9).toString());
    editGuardianPhone->setText(q.value(10).toString());
    editGuardianRelation->setText(q.value(11).toString());
    editNotes->setPlainText(q.value(12).toString());

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

    // 빈 칸은 NULL로 저장 (camera_id는 정수, wearable_id는 문자열)
    auto intOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t.toInt());
    };
    auto textOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t);
    };

    const bool isNew = (selectedResidentId < 0);

    QSqlQuery q;
    if (isNew) {
        // caregiver_id는 요양사 테이블 연동 전이라 제외(기본 NULL)
        q.prepare(QStringLiteral(
            "INSERT INTO residents "
            "(name, room, bed, camera_id, wearable_id, risk_level, admitted_at, "
            " discharge_due, status, guardian_name, guardian_phone, "
            " guardian_relation, notes) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    } else {
        q.prepare(QStringLiteral(
            "UPDATE residents SET name=?, room=?, bed=?, camera_id=?, "
            " wearable_id=?, risk_level=?, admitted_at=?, discharge_due=?, "
            " status=?, guardian_name=?, guardian_phone=?, guardian_relation=?, "
            " notes=? WHERE resident_id=?"));
    }

    q.addBindValue(editName->text().trimmed());
    q.addBindValue(editRoom->text().trimmed());
    q.addBindValue(editBed->text().trimmed());
    q.addBindValue(intOrNull(editCameraId->text()));
    q.addBindValue(textOrNull(editWearableId->text()));
    q.addBindValue(editRiskLevel->currentText());
    q.addBindValue(editAdmittedAt->date());
    q.addBindValue(editDischargeDue->date());
    q.addBindValue(editStatus->currentText());
    q.addBindValue(editGuardianName->text().trimmed());
    q.addBindValue(editGuardianPhone->text().trimmed());
    q.addBindValue(editGuardianRelation->text().trimmed());
    q.addBindValue(editNotes->toPlainText());
    if (!isNew)
        q.addBindValue(selectedResidentId);

    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("저장 실패"), q.lastError().text());
        qDebug() << "입소자 저장 실패:" << q.lastError().text();
        return;
    }

    if (isNew)
        selectedResidentId = q.lastInsertId().toInt();
    int cameraId = editCameraId->text().trimmed().toInt();
    
    // 4채널 중 올바른 채널이고, 서버 소켓이 정상 연결된 상태일 때만 전송
    QTcpSocket* riskSock =
        (cameraId >= 0 && cameraId < 4) ? socketForChannel(cameraId) : nullptr;
    if (editCameraId->text().trimmed().length() > 0 && cameraId >= 0 && cameraId < 4 &&
        riskSock && riskSock->state() == QAbstractSocket::ConnectedState)
    {
        // 1. 위험도 텍스트를 서버가 이해하는 숫자 코드로 매칭 (하:1, 중:2, 상:3)
        int32_t statusVal = 1; 
        QString risk = editRiskLevel->currentText();
        if (risk == QStringLiteral("상")) statusVal = 3;
        else if (risk == QStringLiteral("중")) statusVal = 2;
        else if (risk == QStringLiteral("하")) statusVal = 1;

        // 2. 프로토콜 공용 제어 헤더 조립
        dbj_ctrl_header_t h;
        h.magic = kCtrlMagic;                   // 0xDB4C
        h.version = 0x01;
        h.type = 0x04;
        h.channel = static_cast<uint8_t>(cameraId);
        h.point_count = statusVal;
        h.reserved = 0;

        // 3. 바이트 버퍼 생성 및 데이터 직렬화
        QByteArray pkt;
        pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));

        // 4. 소켓 방출
        riskSock->write(pkt);
        riskSock->flush();
        qDebug() << "➔ [Qt -> 서버] 채널" << (cameraId + 1) << "번 환자 위험도 변경 패킷 전송 완료 (값:" << statusVal << ")";
        
        // 5. 로컬 GUI용 환자 정보 메모리 어레이(patients)도 즉시 동기화
        patients[cameraId].name = editName->text().trimmed();
        patients[cameraId].bed = editRoom->text().trimmed() + QStringLiteral("-") + editBed->text().trimmed();
    }

    refreshResidentTable();
    QMessageBox::information(this, QStringLiteral("저장"),
                             isNew ? QStringLiteral("신규 입소자가 등록되었습니다.")
                                   : QStringLiteral("수정 내용이 저장되었습니다."));
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

    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE residents SET status='퇴원' WHERE resident_id=?"));
    q.addBindValue(selectedResidentId);
    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("퇴원 처리 실패"), q.lastError().text());
        qDebug() << "퇴원 처리 실패:" << q.lastError().text();
        return;
    }

    // 폼의 상태 콤보도 '퇴원'으로 반영
    const int i = editStatus->findText(QStringLiteral("퇴원"));
    if (i >= 0) editStatus->setCurrentIndex(i);

    refreshResidentTable();
    QMessageBox::information(this, QStringLiteral("퇴원 처리"),
                             QStringLiteral("퇴원 처리되었습니다."));
    qDebug() << "퇴원 처리 완료 — ID:" << selectedResidentId;
}
