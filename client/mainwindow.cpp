#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "theme.h"
#include "thememanager.h"
#include "videoview.h"
#include "wintheme.h"
#include "sparkline.h"
#include "mqttqtmanager.h"
#include "alertmatrixpreview.h"
#include <QHostAddress>
#include <QCoreApplication>   // MQTT CA 인증서를 실행 파일 기준 경로에서 찾는다
#include <QFile>
#include <QPixmap>
#include <QDateTime>
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QScrollArea>
#include <QGraphicsDropShadowEffect>
#include <QTextBrowser>
#include <QListWidget>
#include <QPainter>
#include <QIcon>
#include <QLinearGradient>
#include <QResizeEvent>
#include <QPropertyAnimation>
#include <QPair>
#include <QList>
#include <algorithm>
#include <cmath>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>

#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QDateEdit>
#include <QCalendarWidget>
#include <QSlider>
#include "activitychart.h"
#include <QStyle>
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
#include <QTime>
#include <QSet>
#include <QRegularExpression>
#include <QAbstractItemView>
#include <QNetworkInterface>
#include <QProcess>
#include <QLayout>
#include <algorithm>

// 디자인 토큰(kLight/kDark/kAccent…)은 theme.h로 분리했다 — 로그인 화면과 공유.
namespace {

// 폭에 맞춰 자식 위젯을 좌→우로 채우고 넘치면 다음 줄로 접는 레이아웃.
// 입소자 카드 그리드가 창 크기에 따라 열 수를 자동 조절하도록 쓴다(Qt 공식 예제 기반).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent, int margin = 0, int hs = 14, int vs = 14)
        : QLayout(parent), hSpace_(hs), vSpace_(vs) {
        setContentsMargins(margin, margin, margin, margin);
    }
    ~FlowLayout() override { QLayoutItem* it; while ((it = takeAt(0))) delete it; }

    void addItem(QLayoutItem* item) override { items_.append(item); }
    int count() const override { return items_.size(); }
    QLayoutItem* itemAt(int i) const override { return items_.value(i); }
    QLayoutItem* takeAt(int i) override {
        return (i >= 0 && i < items_.size()) ? items_.takeAt(i) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
    void setGeometry(const QRect& r) override { QLayout::setGeometry(r); doLayout(r, false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize s;
        for (QLayoutItem* it : items_) s = s.expandedTo(it->minimumSize());
        const QMargins m = contentsMargins();
        return s + QSize(m.left() + m.right(), m.top() + m.bottom());
    }
private:
    int doLayout(const QRect& rect, bool test) const {
        const QMargins m = contentsMargins();
        const QRect eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = eff.x(), y = eff.y(), lineH = 0;
        for (QLayoutItem* it : items_) {
            const QSize sz = it->sizeHint();
            int nextX = x + sz.width() + hSpace_;
            if (nextX - hSpace_ > eff.right() && lineH > 0) {
                x = eff.x(); y += lineH + vSpace_; nextX = x + sz.width() + hSpace_; lineH = 0;
            }
            if (!test) it->setGeometry(QRect(QPoint(x, y), sz));
            x = nextX; lineH = qMax(lineH, sz.height());
        }
        return y + lineH - rect.y() + m.bottom();
    }
    QList<QLayoutItem*> items_;
    int hSpace_, vSpace_;
};

// 변경 로그에 남길 필드 (라벨, residents 컬럼) — 이 배열만 고치면 로그 대상이 바뀐다.
struct LoggedField { const char* label; const char* column; };
const LoggedField kLoggedFields[] = {
    {"이름", "name"},               {"카메라 채널", "camera_id"},
    {"웨어러블 ID", "wearable_id"}, {"위험도", "risk_level"},
    {"입원일", "admitted_at"},      {"퇴원 예정일", "discharge_due"},
    {"상태", "status"},             {"보호자 이름", "guardian_name"},
    {"보호자 전화", "guardian_phone"}, {"보호자 관계", "guardian_relation"},
    {"특이사항", "notes"},
    };

// 상태 판정 — 웨어러블이 실제로 보내오는 두 값(산소포화도·심박)만 본다.
// SpO2 기준은 임상에서 널리 쓰는 구간: 95% 이상 정상, 90~94% 주의, 90% 미만 위험.
//
// ★ 0은 "값 없음"이지 "이상"이 아니다. 웨어러블을 벗어두면 기기가 심박·SpO2를
//   모두 0으로 보내는데, 이걸 그대로 넣으면 spo2 0 < 90, hr 0 <= 45 양쪽에 걸려
//   미착용 상태 내내 '위험'이 뜬다. 두 값 각각 유효할 때만 판정에 쓴다.
namespace {
enum class VitalLevel { Normal, Warn, Critical };

// 착용 중인지 — 둘 다 0이면 기기가 사람에게 붙어 있지 않다고 본다.
bool vitalWorn(int spo2, int hr) { return spo2 > 0 || hr > 0; }

VitalLevel vitalLevel(int spo2, int hr) {
    const bool hasSpo2 = spo2 > 0;
    const bool hasHr   = hr > 0;
    if ((hasSpo2 && spo2 < 90) || (hasHr && (hr >= 110 || hr <= 45)))
        return VitalLevel::Critical;
    if ((hasSpo2 && spo2 < 95) || (hasHr && (hr >= 100 || hr < 55)))
        return VitalLevel::Warn;
    return VitalLevel::Normal;
}
}  // namespace

// 상태 색상: 정상/주의/위험 판정
QString vitalColor(int spo2, int hr) {
    switch (vitalLevel(spo2, hr)) {
        case VitalLevel::Critical: return kCritical;
        case VitalLevel::Warn:     return kWarn;
        default:                   return kNormal;
    }
}

// 상태 라벨: 정상/주의/위험 (배지 텍스트용)
QString vitalStatusLabel(int spo2, int hr) {
    switch (vitalLevel(spo2, hr)) {
        case VitalLevel::Critical: return QStringLiteral("위험");
        case VitalLevel::Warn:     return QStringLiteral("주의");
        default:                   return QStringLiteral("정상");
    }
}

// ═══════════════════════════════════════════════════════════
//  심각도(severity) 어휘 헬퍼 3종 — ISA-18.2 5단계(02-03-PLAN.md <severity_contract>).
//
//  등급 판정은 여기 한 곳에서만 한다. 색이 필요하면 severityColor(), 도형이
//  필요하면 severityGlyph()를 쓴다. 임계값을 다른 곳에서 다시 판정하지 말 것 —
//  판정 기준이 두 곳에서 갈라지면 같은 화면 안에서 등급이 어긋난다.
// ═══════════════════════════════════════════════════════════

// 바이탈 등급 판정 → QSS severity 속성값 문자열. 기존 vitalLevel()의 임계값을
// 그대로 재사용한다 — 새 기준을 만들지 않는다. 바이탈은 3단계(정상/주의/위험)만
// 판정하므로 "critical"/"medium"/"normal" 중 하나만 돌려준다.
QString vitalSeverity(int spo2, int hr) {
    switch (vitalLevel(spo2, hr)) {
        case VitalLevel::Critical: return QStringLiteral("critical");
        case VitalLevel::Warn:     return QStringLiteral("medium");
        default:                   return QStringLiteral("normal");
    }
}

// 등급 문자열 → theme.h 색 상수. QSS를 받지 않는 커스텀 페인트 위젯(Sparkline 등)
// 전용 통로다 — 이 함수를 거치면 QSS 쪽 severityBadge/severityDot과 같은 등급
// 판정에서 갈라지지 않는다. 알 수 없는 값(무신호 포함)은 중립색을 돌려준다.
QColor severityColor(const QString& severity) {
    if (severity == QStringLiteral("critical")) return QColor(QString::fromLatin1(kCritical));
    if (severity == QStringLiteral("high"))     return QColor(QString::fromLatin1(kHigh));
    if (severity == QStringLiteral("medium"))   return QColor(QString::fromLatin1(kWarn));
    if (severity == QStringLiteral("info"))     return QColor(QString::fromLatin1(kInfo));
    if (severity == QStringLiteral("normal"))   return QColor(QString::fromLatin1(kNormal));
    return QColor(QString::fromLatin1(kTextSub));
}

// 등급 문자열 → 유니코드 도형(<severity_contract> 표). 색만으로는 다섯 등급의
// 그레이스케일 명도가 0.035 폭으로 수렴해(theme_audit.py (c)) 구분이 안 되므로
// 이 도형이 두 번째 채널이다(D-09). stale=true면 severity 값과 무관하게
// 무신호 상태용 빈 원(○)을 돌려준다 — 대기·신호 끊김·미착용 세 상태가
// 공유하는 "심각도가 아니다"라는 표시.
QString severityGlyph(const QString& severity, bool stale = false) {
    if (stale) return QStringLiteral("○");                                // ○ 무신호
    if (severity == QStringLiteral("critical")) return QStringLiteral("✖"); // ✖ 위험
    if (severity == QStringLiteral("high"))     return QStringLiteral("▲"); // ▲ 높음
    if (severity == QStringLiteral("medium"))   return QStringLiteral("◆"); // ◆ 주의
    if (severity == QStringLiteral("info"))     return QStringLiteral("●"); // ● 정보
    if (severity == QStringLiteral("normal"))   return QStringLiteral("✓"); // ✓ 정상
    return QStringLiteral("○");  // 알 수 없는 값 → 무신호와 같은 안전값
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
const char* kDefaultHostA  = "172.20.32.51";
const char* kDefaultHostB  = "172.20.32.50";

// 서버 인덱스(0=Pi A, 1=Pi B) → 저장된 호스트(없으면 기본값).
QString serverHost(int idx) {
    QSettings s;
    return idx == 0 ? s.value(kSettingsHostA, kDefaultHostA).toString()
                    : s.value(kSettingsHostB, kDefaultHostB).toString();
}
// 채널(0~3) → 담당 Pi의 호스트 (블랙박스 클립 URL 등 host가 필요한 곳용).
// 매핑은 MainWindow::serverForChannel과 동일하게 유지할 것 (ch0,1→0 / ch2172.20.32.39,3→1).
QString hostForChannel(int ch) { return serverHost(ch < 2 ? 0 : 1); }

// MQTT 브로커 주소. 영상 서버와 같은 라즈베리에 띄우는 경우가 많아 기본값을
// Pi A 와 같게 뒀지만, 브로커만 따로 두는 구성도 있어 설정으로 분리했다.
const char* kSettingsBrokerHost = "mqtt/brokerHost";
const char* kSettingsBrokerPort = "mqtt/brokerPort";
QString brokerHost() {
    QSettings s;
    return s.value(kSettingsBrokerHost, "172.20.32.51").toString();
}
int brokerPort() {
    QSettings s;
    return s.value(kSettingsBrokerPort, 8883).toInt();   // 8883 = MQTTS(TLS), 평문은 1883
}

// 브로커 검증용 CA 인증서(ca.crt) 위치. 실행 파일 옆의 certs/ 폴더에서 찾는다.
// 상대경로("./certs/ca.crt")를 쓰면 어느 폴더에서 실행했느냐에 따라 못 찾을 수
// 있어서, 실행 파일 기준으로 잡는다.
QString brokerCaPath() {
    QSettings s;
    return s.value(QStringLiteral("mqtt/caCert"),
                   QCoreApplication::applicationDirPath()
                       + QStringLiteral("/certs/ca.crt")).toString();
}

// 이 시간이 지나도록 새 값이 안 오면 화면의 생체값을 "--" 로 되돌린다.
// 웨어러블이 빠졌거나 중계 노드가 죽은 걸 관제사가 알아야 하는데, 마지막 값이
// 계속 떠 있으면 멀쩡한 줄 안다.
constexpr qint64 kVitalStaleMs = 30000;   // 30초

// 입소자별로 보관하는 심박 이력 길이. Sparkline 위젯이 그리는 점 개수(capacity_)와
// 같게 맞춘다 — 카드를 다시 만들 때 이 이력을 그대로 부어넣어 추세를 복원한다.
constexpr int kHrHistoryMax = 40;
}  // namespace
constexpr quint16 kServerPort = 5500;
constexpr int kReconnectDelayMs = 3000;   // 끊김 후 재접속 간격

// 블랙박스 클립 HTTP 서버 포트 (server/src/main.cpp의 kClipHttpPort와 동일하게 유지)
constexpr quint16 kClipHttpPort = 5501;

// NVR(연속녹화) 클립 HTTP 서버 포트 (server/src/config.hpp의 nvr_http_port 기본값과 동일하게 유지)
constexpr quint16 kNvrHttpPort = 5502;

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
                dtItem->setData(Qt::UserRole + 1, channel);
                dtItem->setData(Qt::UserRole + 2, timestampMs);

                logTable->setItem(row, 0, dtItem);
                logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
                logTable->setItem(row, 2, new QTableWidgetItem(eventType));
                logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));
            }
            // 채우기 후 정렬 활성화 → 전체(양쪽 Pi) 최신순 재정렬 (0열 시간 문자열)
            logTable->setSortingEnabled(true);
            logTable->sortItems(0, Qt::DescendingOrder);
            applyLogFilters();   // 현재 필터 조건을 새로 들어온 행에도 적용
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
// 위젯에 부드러운 드롭 섀도를 얹어 카드에 입체감을 준다(모던 대시보드 톤).
// 라이브 영상이 든 영상월 패널엔 쓰지 않는다 — 매 프레임 서브트리를 래스터화해 느려진다.
static void applyCardShadow(QWidget* w, int blur = 22, int dy = 6, int alpha = 70)
{
    auto* sh = new QGraphicsDropShadowEffect(w);
    sh->setBlurRadius(blur);
    sh->setOffset(0, dy);
    sh->setColor(QColor(0, 0, 0, alpha));
    w->setGraphicsEffect(sh);
}

// 경보 중 창 가장자리에 빨강 글로우(비네트)를 잔잔히 숨쉬게 하는 오버레이.
// 두꺼운 테두리가 아니라, 모서리에서 안쪽으로 부드럽게 사라지는 그라데이션 —
// 관제/보안 대시보드에서 흔한 "엣지 경보" 방식. 마우스는 통과, 내부는 투명.
class AlarmOverlay : public QWidget {
public:
    explicit AlarmOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            phase_ += 0.10;                       // 느리고 잔잔한 호흡(≈2초 주기)
            if (phase_ > 6.28318) phase_ -= 6.28318;
            update();
        });
        hide();
    }
    void start() {
        if (!isVisible()) show();
        raise();
        if (!timer_.isActive()) timer_.start(33);
    }
    void stop() { timer_.stop(); hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 부드러운 호흡: 알파만 은은하게 오르내린다(두께·모양 변화 없음).
        const double t = (std::sin(phase_) + 1.0) / 2.0;      // 0..1
        const int a = int((0.20 + 0.30 * t) * 255.0);         // 소프트 0.20..0.50
        const QColor edge(216, 40, 54, a);                    // 차분한 경보 레드
        const QColor clear(216, 40, 54, 0);
        const int W = width(), H = height();
        const int g = qMin(110, qMin(W, H) / 5);              // 글로우가 스며드는 폭

        auto band = [&](const QRect& r, const QPointF& from, const QPointF& to) {
            QLinearGradient lg(from, to);
            lg.setColorAt(0.0, edge);
            lg.setColorAt(1.0, clear);
            p.fillRect(r, lg);
        };
        band(QRect(0, 0, W, g),        QPointF(0, 0),   QPointF(0, g));         // 상
        band(QRect(0, H - g, W, g),    QPointF(0, H),   QPointF(0, H - g));     // 하
        band(QRect(0, 0, g, H),        QPointF(0, 0),   QPointF(g, 0));         // 좌
        band(QRect(W - g, 0, g, H),    QPointF(W, 0),   QPointF(W - g, 0));     // 우

        // 가장자리에 아주 얇은 1px 라인 한 스푼 — 프레임이 또렷하게 잡힌다.
        p.setPen(QPen(QColor(230, 55, 66, int(a * 0.8)), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    }

private:
    QTimer timer_;
    double phase_ = 0.0;
};

namespace {
// 좌측 네비 아이콘 — 외부 리소스 없이 QPainter로 그린다.
// 이모지를 쓰면 폰트마다 톤·크기가 제각각이라 관제 UI에서 장난감처럼 보인다.
// 선 굵기 하나로 통일된 단색 아이콘이라 팔레트가 바뀌면 색만 다시 입히면 된다.
QPixmap navIconPixmap(int kind, const QColor& c, int px = 20)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal m = 2.5, w = px - 2 * m;
    switch (kind) {
        case 0: {   // 실시간 관제 — 2×2 영상 그리드
            const qreal g = 1.8, s = (w - g) / 2.0;
            for (int r = 0; r < 2; ++r)
                for (int col = 0; col < 2; ++col)
                    p.drawRoundedRect(
                        QRectF(m + col * (s + g), m + r * (s + g), s, s), 1.5, 1.5);
            break;
        }
        case 1: {   // 이벤트 기록 — 점 + 줄 목록
            for (int i = 0; i < 3; ++i) {
                const qreal y = m + 1.5 + i * (w - 3) / 2.0;
                p.setBrush(c);
                p.drawEllipse(QPointF(m + 1.2, y), 1.15, 1.15);
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(m + 5.0, y), QPointF(m + w, y));
            }
            break;
        }
        case 2: {   // 일일 리포트 — 문서(가로줄)
            p.drawRoundedRect(QRectF(m, m, w, w), 2.0, 2.0);
            for (int i = 0; i < 3; ++i) {
                const qreal y = m + w * 0.30 + i * w * 0.22;
                p.drawLine(QPointF(m + w * 0.2, y), QPointF(m + w * 0.8, y));
            }
            break;
        }
        case 3: {   // 입소자 관리 — 사람
            p.drawEllipse(QRectF(m + w * 0.29, m + w * 0.04, w * 0.42, w * 0.42));
            p.drawArc(QRectF(m + w * 0.06, m + w * 0.54, w * 0.88, w * 0.84), 0, 180 * 16);
            break;
        }
        default: {  // 장치 설정 — 카메라 바디 + 렌즈
            p.drawRoundedRect(QRectF(m, m + w * 0.20, w, w * 0.62), 2.5, 2.5);
            p.drawEllipse(QRectF(m + w * 0.33, m + w * 0.36, w * 0.34, w * 0.32));
            p.drawLine(QPointF(m + w * 0.30, m + w * 0.20),
                       QPointF(m + w * 0.40, m + w * 0.06));
            p.drawLine(QPointF(m + w * 0.40, m + w * 0.06),
                       QPointF(m + w * 0.62, m + w * 0.06));
            break;
        }
    }
    return pm;
}
}  // namespace

// 좌측 네비 레일 — 아이콘+라벨 5개, 접으면 아이콘만. 상태는 QSettings에 남는다.
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
        QStringLiteral("일일 리포트"), QStringLiteral("입소자 관리"),
        QStringLiteral("장치 설정")};
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
    auto* body = new QHBoxLayout();
    body->setContentsMargins(18, 18, 18, 18);
    body->setSpacing(18);
    body->addWidget(buildVideoWall(), 1);
    body->addWidget(buildVitalsPanel(), 0);

    // 경보 배너(#alertBanner) — dashboardTab 최상단, body 위. 활성 경보가
    // 있을 때만 보이고 0건이면 숨는다. 좌우는 body와 같은 세로선에 맞추고
    // (18px) 상하는 최소로 둔다 — QSS가 이미 padding 24px 32px를 준다
    // (ALERT-03 / D-01).
    alertBanner_ = new AlertBanner();
    alertBanner_->setContentsMargins(18, 12, 18, 0);

    auto* dashboardOuter = new QVBoxLayout();
    dashboardOuter->setContentsMargins(0, 0, 0, 0);
    dashboardOuter->setSpacing(0);
    dashboardOuter->addWidget(alertBanner_);
    dashboardOuter->addLayout(body, 1);

    auto* dashboardTab = new QWidget();
    dashboardTab->setLayout(dashboardOuter);
    contentStack->addWidget(dashboardTab);

    contentStack->addWidget(buildEventLogTab());        // 2: 이벤트 기록
    contentStack->addWidget(buildReportPage());         // 3: 일일 리포트
    contentStack->addWidget(buildDbTab());              // 4: 입소자 관리
    contentStack->addWidget(buildDeviceSettingsTab());  // 5: 장치 설정(카메라 + 알림)

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

    resize(1600, 940);
    setMinimumSize(1340, 760);
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
    auto* logo = new QLabel(QStringLiteral("다보이조"));
    logo->setObjectName("logo");

    lay->addWidget(logo);
    lay->addStretch();

    // ── 실시간 관제 액션 — 방송(인터콤) ──
    // 경보 해제는 헤더가 아니라 "경보 배너"(경보 시에만 표시)로 옮겼다.
    micButton = new QPushButton(QStringLiteral("🎤 방송"));
    micButton->setObjectName("micButton");
    micButton->setCursor(Qt::PointingHandCursor);
    connect(micButton, &QPushButton::pressed, this, &MainWindow::onMicPressed);
    connect(micButton, &QPushButton::released, this, &MainWindow::onMicReleased);
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
    helpButton->setToolTip(QStringLiteral("도움말 — 기능 설명"));
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

    // 이름 첫 글자를 딴 원형 배지 — 누가 로그인해 있는지 한눈에 보이게
    userAvatarLabel = new QLabel();
    userAvatarLabel->setObjectName("userAvatar");
    userAvatarLabel->setFixedSize(30, 30);
    userAvatarLabel->setAlignment(Qt::AlignCenter);
    userAvatarLabel->setText(currentUser.name.left(1));

    userNameLabel = new QLabel();
    userNameLabel->setObjectName("userName");
    userNameLabel->setText(currentUser.name);
    userNameLabel->setToolTip(QStringLiteral("%1 (%2)")
                                  .arg(currentUser.name, currentUser.loginId));

    logoutButton = new QPushButton(QStringLiteral("로그아웃"));
    logoutButton->setObjectName("logoutButton");
    logoutButton->setCursor(Qt::PointingHandCursor);
    connect(logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);

    ucl->addWidget(userAvatarLabel);
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
//  도움말 — 앱의 모든 기능을 설명하는 창(한화 웹UI의 '도움말'과 유사)
// ═══════════════════════════════════════════════════════════
void MainWindow::onHelpClicked()
{
    if (!helpDialog) {
        helpDialog = new QDialog(this);
        helpDialog->setObjectName("panel");
        helpDialog->setWindowTitle(QStringLiteral("도움말 — 기능 설명"));
        helpDialog->resize(880, 660);
        helpDialog->setMinimumSize(640, 440);
        enableDarkTitleBar(helpDialog);
        auto* h = new QHBoxLayout(helpDialog);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        // 좌측 주제 목록
        helpList = new QListWidget();
        helpList->setObjectName(QStringLiteral("helpList"));
        helpList->setFixedWidth(200);
        helpList->addItems({
            QStringLiteral("개요"),
            QStringLiteral("상단 헤더"),
            QStringLiteral("실시간 관제 및 제어"),
            QStringLiteral("이벤트 기록"),
            QStringLiteral("일일 리포트"),
            QStringLiteral("입소자 관리"),
            QStringLiteral("장치 설정"),
        });
        h->addWidget(helpList);

        // 우측 내용
        helpBrowser = new QTextBrowser();
        helpBrowser->setObjectName(QStringLiteral("helpBrowser"));
        helpBrowser->setOpenExternalLinks(false);
        h->addWidget(helpBrowser, 1);

        connect(helpList, &QListWidget::currentRowChanged, this,
                &MainWindow::renderHelpTopic);
        helpList->setCurrentRow(0);
    }
    renderHelpTopic(helpList ? helpList->currentRow() : 0);  // 현재 테마 색으로 갱신
    helpDialog->show();
    helpDialog->raise();
    helpDialog->activateWindow();
}

// 선택된 도움말 주제를 현재 테마 색으로 렌더한다.
void MainWindow::renderHelpTopic(int idx)
{
    if (!helpBrowser) return;
    if (idx < 0) idx = 0;

    const QString A  = QString::fromLatin1(kAccent);
    const QString T  = QString::fromLatin1(kTextMain);
    const QString S  = QString::fromLatin1(kTextSub);
    const QString BD = QString::fromLatin1(kBorder);

    auto li = [](const QString& k, const QString& d) {
        return QStringLiteral("<p style='margin:9px 0;'><b>%1</b><br>%2</p>").arg(k, d);
    };
    QString title, body;
    switch (idx) {
    case 0:
        title = QStringLiteral("개요");
        body = QStringLiteral(
            "<p>다보이조는 요양원 통합 모니터링 관제 프로그램입니다. "
            "실시간 영상 관제, 낙상·침상이탈 경보, 웨어러블 생체신호, 블랙박스 기록, "
            "입소자 관리, 장치 설정(카메라·알림)을 한 화면에서 다룹니다.</p>")
          + li(QStringLiteral("화면 구성"),
               QStringLiteral("왼쪽 메뉴에서 실시간 관제 · 이벤트 기록 · 일일 리포트 · 입소자 관리 · 장치 설정으로 이동합니다. "
                              "메뉴 위 ☰ 버튼으로 접었다 펼 수 있습니다."))
          + li(QStringLiteral("사용 팁"),
               QStringLiteral("왼쪽 목록에서 주제를 고르면 해당 기능 설명이 여기에 표시됩니다."));
        break;
    case 1:
        title = QStringLiteral("상단 헤더");
        body = li(QStringLiteral("영상 서버 상태등"), QStringLiteral("초록=정상 연결, 빨강=연결 끊김. 끊기면 자동 재접속을 시도합니다."))
             + li(QStringLiteral("실시간 시계"), QStringLiteral("현재 시각(관제 기록 기준)."))
             + li(QStringLiteral("도움말"), QStringLiteral("이 창을 엽니다."))
             + li(QStringLiteral("테마 전환(🌙/☀)"), QStringLiteral("다크(야간 관제)·라이트(주간) 전환."))
             + li(QStringLiteral("계정 · 로그아웃"), QStringLiteral("로그인 사용자 표시. 로그아웃 시 로그인 화면으로 복귀."));
        break;
    case 2:
        title = QStringLiteral("실시간 관제 및 제어");
        body = li(QStringLiteral("4채널 영상"), QStringLiteral("병상별 실시간 영상. 낙상·침상이탈 발생 시 해당 칸이 빨간 테두리로 강조됩니다."))
             + li(QStringLiteral("🎤 방송"), QStringLiteral("누르고 있는 동안 현장으로 음성 송출(인터콤). 떼면 종료."))
             + li(QStringLiteral("경보 해제"), QStringLiteral("평상시엔 차분한 아웃라인, 경보 시 빨강 강조. 누르면 낙상/침상이탈 경보를 일괄 해제하고 현장 사이렌·LED를 끕니다."))
             + li(QStringLiteral("웨어러블 생체신호"), QStringLiteral("우측 패널에 채널별 산소포화도·심박과 심박 추세 그래프. 정상/주의/위험에 따라 색이 바뀝니다."));
        break;
    case 3:
        title = QStringLiteral("이벤트 기록");
        body = li(QStringLiteral("필터"), QStringLiteral("날짜 범위·이벤트 종류를 바꾸면 즉시 목록에 반영(별도 검색 버튼 없음)."))
             + li(QStringLiteral("로그 표"), QStringLiteral("낙상=빨강, 침상이탈=주황. 상태는 미확인=빨강/확인=초록으로 구분."))
             + li(QStringLiteral("블랙박스 재생"), QStringLiteral("표의 이벤트를 더블클릭하면 우측 플레이어에서 그 시점 영상을 바로 재생하고 ‘확인’ 처리됩니다."));
        break;
    case 4:
        title = QStringLiteral("일일 리포트");
        body = li(QStringLiteral("날짜 선택"), QStringLiteral("좌측 달력에서 날짜를 고르면 그 날의 기록을 조회합니다. 자료가 없는 미래 날짜는 선택되지 않으며, ‘오늘’ 버튼으로 돌아옵니다."))
             + li(QStringLiteral("입소자 탭"), QStringLiteral("상단 이름 탭으로 사람을 전환합니다. 리포트는 ‘날짜 + 입소자’ 한 명 단위입니다."))
             + li(QStringLiteral("지표"), QStringLiteral("누워있는 시간·활동량(만보기)·케어시간·이벤트 횟수. 서버가 쌓는 bed_sessions·activity_minute·care_logs·events 기준입니다."));
        break;
    case 5:
        title = QStringLiteral("입소자 관리");
        body = li(QStringLiteral("상단 요약"), QStringLiteral("재원 인원·위험도(상/중/하) 분포·채널 배정 수를 한눈에."))
             + li(QStringLiteral("재원/전체/퇴원 필터"), QStringLiteral("좌측 목록을 상태별로 전환. 이름 검색도 가능."))
             + li(QStringLiteral("목록 → 상세"), QStringLiteral("행을 클릭하면 우측에서 바로 편집(팝업 없음). 행 왼쪽 색 띠는 위험도(상=빨강/중=주황/하=초록)."))
             + li(QStringLiteral("＋ 신규 등록 / 저장 / 퇴원 처리"), QStringLiteral("입소자 추가·수정·퇴원(재입원). 변경 내역은 입원 이력에 기록됩니다."));
        break;
    case 6:
    default:
        title = QStringLiteral("장치 설정");
        body = li(QStringLiteral("상단 [카메라] / [알림] 전환"), QStringLiteral("카메라(연결·ROI·이미지)와 알림 노드 설정을 한 화면에서 서브탭으로 오갑니다."))
             + li(QStringLiteral("카메라 · 채널 레일(CH1~4)"), QStringLiteral("상단에서 채널 선택. 연결 상태와 지정된 침대 수가 배지로 표시되고, 아래 컨트롤과 우측 영상이 그 채널로 묶입니다."))
             + li(QStringLiteral("카메라 · 연결"), QStringLiteral("CCTV IP·계정·비밀번호 입력 후 연결. ‘같은 망 카메라 검색’으로 자동 탐색."))
             + li(QStringLiteral("카메라 · ROI(침대)"), QStringLiteral("‘침대 추가’ → 우측 영상 클릭으로 침대 영역을 그리고 더블클릭으로 완료. 한 채널에 침대를 여러 개(최대 8개) 지정할 수 있고, 각 침대가 낙상·침상이탈 판정 기준이 됩니다."))
             + li(QStringLiteral("카메라 · 침대·입소자 매핑"), QStringLiteral("침대 목록에서 그 침대의 입소자를 지정하면, 그 침대에서 감지된 사람에게 이름이 붙어 낙상·이탈 알림에 ‘침대 2 김복순’처럼 표시됩니다. 서버가 사람을 특정하지 못하면 ‘신원 미상’으로 알립니다(추적 ID는 신원이 아니라서 확정할 수 없습니다)."))
             + li(QStringLiteral("카메라 · 이미지"), QStringLiteral("밝기·대비·채도 슬라이더 후 ‘적용’. 우측에 적용 전/적용 후(실시간) 비교. 실시간 영상을 클릭하면 그 지점에 초점을 맞춥니다."))
             + li(QStringLiteral("알림 · 밝기/음량"), QStringLiteral("대상 알림 노드를 고르고 LED 밝기·스피커 음량을 조절합니다. 미리보기가 밝기를 바로 보여줍니다."))
             + li(QStringLiteral("알림 · 테스트/적용"), QStringLiteral("‘테스트’는 현재 값으로 현장 LED에 문구 1회 + 짧은 소리를 냅니다. ‘적용’은 그 값을 평상시 설정으로 저장합니다."));
        break;
    }

    const QString html = QStringLiteral(
        "<div style='font-family:\"Segoe UI\",\"맑은 고딕\",\"Malgun Gothic\",\"Apple SD Gothic Neo\",\"Noto Sans CJK KR\",\"Noto Sans KR\",sans-serif; font-size:14px; color:%1;'>"
        "<h1 style='color:%2; margin:0 0 10px;'>%3</h1>"
        "<hr style='border:none; border-top:1px solid %4;'>"
        "<div style='line-height:155%;'>%5</div></div>")
        .arg(T, A, title, BD, body);
    helpBrowser->setHtml(html);
    // 배경/여백은 base.qss의 QTextBrowser#helpBrowser 규칙이 담당한다(테마 토글 시 자동 갱신).
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
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(0);

    // 방송·경보해제 버튼은 상단 헤더로 올렸다 → 여기선 영상 4개가 패널을 꽉 채운다.
    videoGrid = new QGridLayout();
    videoGrid->setSpacing(12);   // 라운드 카드가 숨 쉴 만큼의 간격(모던 대시보드 톤)
    for (int ch = 0; ch < 4; ++ch)
        videoCards[ch] = buildVideoCard(ch);
    outer->addLayout(videoGrid, 1);

    setVideoFocus(-1);   // 초기: 균등 2×2 배치
    return panel;
}

QWidget* MainWindow::buildVideoCard(int channel)
{
    auto* card = new QFrame();
    card->setObjectName("videoCard");
    // 스포트라이트 시 작은 칸으로도 줄어들 수 있게 최소 크기를 낮게 잡는다.
    card->setMinimumSize(140, 96);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // 영상 영역 — VideoView가 프레임 + NVR 오버레이(CH 태그/LIVE) + ROI를 담당.
    // 오버레이는 "CH1"만 — 병상·이름은 표시하지 않는다(overlayInfo 비움).
    auto* video = new VideoView(channel);
    video->setObjectName("video");
    video->setCornerRadius(11);   // 카드(#videoCard 12px) 안쪽에 딱 맞게
    channelViews[channel] = video;
    connect(video, &VideoView::roiCompleted, this, &MainWindow::onRoiCompleted);
    connect(video, &VideoView::drawModeChanged, this,
            [this](int, bool on) {
                roiDrawing = on;
                if (roiButton)
                    roiButton->setText(on ? QStringLiteral("취소")
                                          : QStringLiteral("침대 추가"));
            });
    lay->addWidget(video, 1);

    return card;
}

// 감지된 채널을 좌측 대형으로, 나머지 3개는 우측에 작게 세로로 배치한다(스포트라이트).
// channel<0 이면 균등 2×2로 복귀. 팝업 없이 그리드만 재배치한다.
void MainWindow::setVideoFocus(int channel)
{
    if (!videoGrid) return;
    if (channel == focusedChannel_) return;   // 이미 그 상태면 재배치 생략
    focusedChannel_ = channel;

    for (int ch = 0; ch < 4; ++ch)
        if (videoCards[ch]) videoGrid->removeWidget(videoCards[ch]);
    for (int i = 0; i < 4; ++i) {             // 스트레치 초기화
        videoGrid->setColumnStretch(i, 0);
        videoGrid->setRowStretch(i, 0);
    }

    if (channel < 0 || channel >= 4) {
        // 균등 2×2
        videoGrid->addWidget(videoCards[0], 0, 0);
        videoGrid->addWidget(videoCards[1], 0, 1);
        videoGrid->addWidget(videoCards[2], 1, 0);
        videoGrid->addWidget(videoCards[3], 1, 1);
        videoGrid->setColumnStretch(0, 1);
        videoGrid->setColumnStretch(1, 1);
        videoGrid->setRowStretch(0, 1);
        videoGrid->setRowStretch(1, 1);
    } else {
        // 스포트라이트: 좌측 대형(3행 span) + 우측 작은 3개 세로
        videoGrid->addWidget(videoCards[channel], 0, 0, 3, 1);
        int r = 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (ch == channel) continue;
            videoGrid->addWidget(videoCards[ch], r, 1, 1, 1);
            ++r;
        }
        videoGrid->setColumnStretch(0, 3);   // 대형 ≈ 75%
        videoGrid->setColumnStretch(1, 1);   // 작은 열 ≈ 25%
        videoGrid->setRowStretch(0, 1);
        videoGrid->setRowStretch(1, 1);
        videoGrid->setRowStretch(2, 1);
    }
}

QWidget* MainWindow::buildVitalsPanel()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");
    // 바이탈 패널은 폭 고정 → 창을 키우면 남는 폭이 전부 영상 월로 간다.
    panel->setMinimumWidth(300);
    panel->setMaximumWidth(316);

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
    vitalListLayout_ = new QVBoxLayout(inner);
    vitalListLayout_->setContentsMargins(0, 0, 6, 0);
    vitalListLayout_->setSpacing(10);
    // 카드 개수는 재원 입소자 수에 따라 달라진다(한 채널에 여러 명일 수 있음).
    // 생성자에서 loadPatientsFromDb() 를 먼저 부르므로 여기서 목록을 알 수 있다.
    rebuildVitalCards();

    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    return panel;
}

// 바이탈 카드 목록을 입소자 구성에 맞춰 다시 만든다(D-04 — diff 방식).
// 구성이 안 바뀌면 위젯을 하나도 만들지도 지우지도 않는다. 심박 이력은
// hrHistory_(위젯 밖)에 있어서 타일이 새로 만들어져도 살아남는다.
void MainWindow::rebuildVitalCards()
{
    if (!vitalListLayout_) return;   // 아직 패널을 만들기 전(생성자 초기 단계)

    // ── 목표 키 순서 목록: (키, 이름, 병상표기) 3튜플. 순서가 곧 화면 배치 순서다. ──
    struct TargetEntry { int key; QString name; QString bedText; };
    QVector<TargetEntry> target;
    target.reserve(8);
    for (int ch = 0; ch < 4; ++ch) {
        const QString bedText = QStringLiteral("채널 %1").arg(ch + 1);
        const QVector<int>& ids = residentsByChannel_[ch];

        // 아무도 배정되지 않은 채널도 자리를 남긴다 — 카드가 통째로 사라지면
        // 관제사가 그 채널을 잊는다. 음수 키라 값이 안 들어와 "대기"로 뜬다.
        if (ids.isEmpty()) {
            target.append({-(ch + 1), QStringLiteral("미배정"), bedText});
            continue;
        }
        for (int rid : ids)
            target.append({rid, residentInfo_.value(rid).name, bedText});
    }

    // ── 사라진 키를 제거한다. 순회 중 해시를 수정하지 않도록 대상 키를 먼저
    //    모은 뒤 별도 루프에서 지운다. ──
    QSet<int> targetKeys;
    for (const auto& t : target) targetKeys.insert(t.key);
    QVector<int> staleKeys;
    for (auto it = vitalTiles_.constBegin(); it != vitalTiles_.constEnd(); ++it)
        if (!targetKeys.contains(it.key())) staleKeys.append(it.key());
    for (int key : staleKeys) {
        if (VitalTile* tile = vitalTiles_.take(key)) {
            vitalListLayout_->removeWidget(tile);
            tile->setParent(nullptr);
            tile->deleteLater();
        }
    }

    // ── 목표 목록을 순서대로 훑으며 생성 또는 갱신한다. ──
    for (const auto& t : target) {
        VitalTile* tile = vitalTiles_.value(t.key);
        if (!tile) {
            tile = buildVitalCard(t.key, t.name, t.bedText);
            vitalListLayout_->addWidget(tile, 1);
        } else {
            // 조건 없이 호출한다 — PD-01의 멱등 가드가 변화 없을 때를 흡수하고,
            // 같은 입소자가 다른 채널로 옮겨져 병상 표기만 바뀐 경우를 놓치지 않는다.
            tile->setIdentity(t.name, t.bedText);
        }
    }

    // ── 배치 순서를 목표 목록과 일치시킨다. 살아남은 타일이 섞여 있으면
    //    인덱스가 어긋날 수 있다. ──
    for (int i = 0; i < target.size(); ++i) {
        VitalTile* tile = vitalTiles_.value(target[i].key);
        if (!tile) continue;
        if (vitalListLayout_->indexOf(tile) != i) {
            vitalListLayout_->removeWidget(tile);
            vitalListLayout_->insertWidget(i, tile, 1);
        }
    }

    updateVitals();   // 새로 만든 위젯에 현재 값·색을 즉시 반영
}

VitalTile* MainWindow::buildVitalCard(int key, const QString& name, const QString& bedText)
{
    auto* tile = new VitalTile();
    applyCardShadow(tile, 20, 5, 60);   // 바이탈 카드에 은은한 입체감(static이라 타일 밖에서 건다 — PD-02)
    tile->setIdentity(name, bedText);
    // 카드를 다시 만들어도 그래프가 리셋되지 않도록 보관해둔 이력을 다시 부어넣는다.
    // (다른 입소자가 추가·퇴원했다고 이 사람 추세가 사라지면 안 된다)
    for (double v : hrHistory_.value(key)) tile->pushHeartRateSample(v);
    vitalTiles_[key] = tile;
    return tile;
}

// ═══════════════════════════════════════════════════════════
//  TAB2: 이벤트 기록 — 상단 요약 카드 + 필터 + [로그 표 | 인라인 블랙박스]
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildEventLogTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("이벤트 기록"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 필터 바
    outer->addWidget(buildSearchFilters());

    // 본문: 좌측 로그 표 / 우측 인라인 블랙박스 재생
    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildLogTable(), 5);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);
    auto* bbCap = new QLabel(QStringLiteral("블랙박스"));
    bbCap->setObjectName("panelTitle");
    right->addWidget(bbCap);
    auto* bbHint = new QLabel(
        QStringLiteral("왼쪽 표의 이벤트를 더블클릭하면 여기서 바로 재생됩니다."));
    bbHint->setObjectName("subtitle");
    bbHint->setWordWrap(true);
    right->addWidget(bbHint);
    right->addWidget(buildBlackboxPlayer(), 1);

    auto* actionRow = new QHBoxLayout();
    nvrJumpButton = new QPushButton(QStringLiteral("이 시점 NVR에서 이어보기"));
    nvrJumpButton->setObjectName("nvrJumpButton");
    nvrJumpButton->setCursor(Qt::PointingHandCursor);
    nvrJumpButton->setEnabled(false);   // 로그에서 이벤트를 열기 전까진 대상이 없음
    connect(nvrJumpButton, &QPushButton::clicked, this, &MainWindow::jumpToNvrContext);
    actionRow->addWidget(nvrJumpButton, 1);

    clipDownloadButton = new QPushButton(QStringLiteral("다운로드"));
    clipDownloadButton->setObjectName("clipDownloadButton");
    clipDownloadButton->setCursor(Qt::PointingHandCursor);
    clipDownloadButton->setEnabled(false);   // 재생된 클립이 있어야 저장할 게 있음
    connect(clipDownloadButton, &QPushButton::clicked, this, &MainWindow::downloadCurrentClip);
    actionRow->addWidget(clipDownloadButton);
    right->addLayout(actionRow);

    right->addWidget(buildNvrBrowser());
    body->addLayout(right, 4);

    outer->addLayout(body, 1);

    refreshEventLog();   // 초기 요약 값(0건) 세팅
    return panel;
}

// 일일 리포트 — [좌: 달력] | [우: 이름 탭 + 그 사람의 그 날 지표]
//
// 예전엔 "오늘" 만 볼 수 있는 케어 타임 대시보드였다. 리포트는 지난 날짜를
// 되짚어 보는 게 본질이라 날짜 선택을 앞에 세운다. 채널이 아니라 입소자 단위인
// 이유: 침대마다 사람을 매핑하면 한 채널에 여러 명이 들어와 4칸 고정이 안 맞는다.
QWidget* MainWindow::buildReportPage()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("일일 리포트"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    auto* sub = new QLabel(
        QStringLiteral("날짜를 선택하면 그 날의 입소자별 기록을 조회합니다"));
    sub->setObjectName("subtitle");
    outer->addWidget(sub);
    outer->addSpacing(10);

    // ── 좌우 2단: 달력(고정 폭) | 리포트 본문(남는 공간 전부) ──
    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildReportCalendar(), 0);

    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    // 선택한 날짜를 우측에도 크게 — 달력에서 눈을 떼도 어느 날 자료인지 보이게.
    reportDateLabel = new QLabel();
    reportDateLabel->setObjectName("reportDate");
    right->addWidget(reportDateLabel);
    right->addWidget(buildReportDetail(), 1);

    body->addLayout(right, 1);
    outer->addLayout(body, 1);

    reloadReportResidents();            // 이름 탭 채우기 → 첫 입소자 자동 선택
    onReportDateChanged(reportDate_);   // 날짜 라벨 + 지표 초기 조회
    return panel;
}

// 좌측 날짜 선택 칼럼 — 달력 + (앞으로) PDF·AI 요약 버튼이 붙을 자리.
QWidget* MainWindow::buildReportCalendar()
{
    auto* col = new QFrame();
    col->setObjectName("careCard");
    col->setFixedWidth(320);
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(14, 14, 14, 14);
    lay->setSpacing(10);

    auto* cap = new QLabel(QStringLiteral("날짜 선택"));
    cap->setObjectName("careBigCap");
    lay->addWidget(cap);

    reportCalendar = new QCalendarWidget();
    reportCalendar->setObjectName("reportCalendar");
    reportCalendar->setGridVisible(false);
    reportCalendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);  // 주차 번호 숨김
    reportCalendar->setHorizontalHeaderFormat(QCalendarWidget::ShortDayNames);
    reportCalendar->setSelectedDate(reportDate_);
    // 미래 날짜는 볼 자료가 없다 — 아예 못 고르게 막는다.
    reportCalendar->setMaximumDate(QDate::currentDate());
    lay->addWidget(reportCalendar);

    // 오늘로 되돌아오는 버튼 — 과거를 뒤지다 보면 오늘 찾아 돌아오기가 번거롭다.
    auto* today = new QPushButton(QStringLiteral("오늘"));
    today->setObjectName("reportTodayBtn");
    today->setCursor(Qt::PointingHandCursor);
    connect(today, &QPushButton::clicked, this, [this] {
        reportCalendar->setSelectedDate(QDate::currentDate());
    });
    lay->addWidget(today);

    // TODO(리포트): 이 아래에 [PDF 내보내기] [AI 요약] 버튼이 들어간다.
    lay->addStretch(1);

    connect(reportCalendar, &QCalendarWidget::selectionChanged, this, [this] {
        onReportDateChanged(reportCalendar->selectedDate());
    });
    return col;
}

// 우측 상세 — [이름 탭] + [지표 타일 4개] + (앞으로) 그래프·이벤트 표·AI 요약.
QWidget* MainWindow::buildReportDetail()
{
    auto* host = new QFrame();
    host->setObjectName("careCard");
    auto* lay = new QVBoxLayout(host);
    lay->setContentsMargins(16, 14, 16, 16);
    lay->setSpacing(12);

    // ── 입소자 이름 탭 ──
    // 버튼은 reloadReportResidents()가 DB를 보고 만든다. 여기선 담을 줄만 잡는다.
    auto* tabRow = new QWidget();
    residentTabLayout = new QHBoxLayout(tabRow);
    residentTabLayout->setContentsMargins(0, 0, 0, 0);
    residentTabLayout->setSpacing(6);
    residentTabLayout->addStretch(1);   // 버튼은 이 스트레치 앞에 끼워 넣는다
    lay->addWidget(tabRow);

    reportResidentMeta = new QLabel(QStringLiteral("—"));
    reportResidentMeta->setObjectName("careMeta");
    lay->addWidget(reportResidentMeta);

    // ── 지표 타일 4개 ──
    // 큰 숫자 + 아래 보조 문구. 라벨만 멤버로 잡아두고 갱신 때 텍스트만 바꾼다.
    auto makeTile = [](const QString& cap, QLabel** val, QLabel** sub) {
        auto* tile = new QFrame();
        tile->setObjectName("reportTile");
        auto* v = new QVBoxLayout(tile);
        v->setContentsMargins(14, 12, 14, 12);
        v->setSpacing(2);
        auto* c = new QLabel(cap);
        c->setObjectName("careBigCap");
        *val = new QLabel(QStringLiteral("—"));
        (*val)->setObjectName("reportTileVal");
        *sub = new QLabel(QStringLiteral(" "));
        (*sub)->setObjectName("careMiniCap");
        v->addWidget(c);
        v->addWidget(*val);
        v->addWidget(*sub);
        return tile;
    };

    auto* tiles = new QGridLayout();
    tiles->setSpacing(12);
    tiles->addWidget(makeTile(QStringLiteral("누워있는 시간"),
                              &tileLyingVal, &tileLyingSub), 0, 0);
    tiles->addWidget(makeTile(QStringLiteral("활동량"),
                              &tileActivityVal, &tileActivitySub), 0, 1);
    tiles->addWidget(makeTile(QStringLiteral("케어시간"),
                              &tileCareVal, &tileCareSub), 0, 2);
    tiles->addWidget(makeTile(QStringLiteral("이벤트"),
                              &tileEventVal, &tileEventSub), 0, 3);
    for (int c = 0; c < 4; ++c) tiles->setColumnStretch(c, 1);
    lay->addLayout(tiles);

    // ── 24시간 활동량 그래프 ──
    auto* chartCap = new QLabel(QStringLiteral("시간별 활동량"));
    chartCap->setObjectName("careBigCap");
    lay->addWidget(chartCap);

    activityChart = new ActivityChart();
    lay->addWidget(activityChart, 1);

    // TODO(리포트): 이 아래에 이벤트 타임라인 · AI 요약이 온다.
    return host;
}

// 재원 입소자로 이름 탭을 다시 만든다. 입소자 관리에서 사람이 바뀌면 다시 부른다.
//
// ※ 지금은 "현재 재원"만 본다. 과거 날짜를 보면서 그 시점 재원자로 목록을 맞추려면
//   admissions(입원 에피소드)를 조회해야 하는데, 그건 리포트가 자리 잡은 뒤에.
void MainWindow::reloadReportResidents()
{
    if (!residentTabLayout) return;

    for (auto* b : residentTabBtns) { residentTabLayout->removeWidget(b); b->deleteLater(); }
    residentTabBtns.clear();
    residentTabIds.clear();

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT resident_id, name FROM residents WHERE status='재원' "
            "ORDER BY camera_id, bed, resident_id"))) {
        qDebug() << "리포트 입소자 목록 조회 실패:" << q.lastError().text();
        return;
    }
    while (q.next()) {
        const int id = q.value(0).toInt();
        auto* b = new QPushButton(q.value(1).toString());
        b->setObjectName("residentTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, id] { onReportResidentChanged(id); });
        // 마지막 stretch 앞에 끼워 넣어 버튼들이 왼쪽으로 몰리게 한다.
        residentTabLayout->insertWidget(residentTabLayout->count() - 1, b);
        residentTabBtns.push_back(b);
        residentTabIds.push_back(id);
    }

    // 보던 사람이 목록에서 사라졌으면 첫 사람으로 되돌린다.
    if (!residentTabIds.contains(reportResidentId_))
        reportResidentId_ = residentTabIds.isEmpty() ? -1 : residentTabIds.first();
    onReportResidentChanged(reportResidentId_);
}

void MainWindow::onReportResidentChanged(int residentId)
{
    reportResidentId_ = residentId;

    // 선택된 탭만 눌린 상태로 — QButtonGroup 없이 직접 맞춘다(탭이 매번 재생성됨).
    for (int i = 0; i < residentTabBtns.size(); ++i)
        residentTabBtns[i]->setChecked(residentTabIds.value(i) == residentId);

    if (reportResidentMeta) {
        QString meta = QStringLiteral("—");
        if (residentId > 0) {
            QSqlQuery q;
            q.prepare(QStringLiteral(
                "SELECT room, bed, COALESCE(risk_level,'—') FROM residents WHERE resident_id=?"));
            q.addBindValue(residentId);
            if (q.exec() && q.next())
                meta = QStringLiteral("%1호 · %2 · 위험도 %3")
                           .arg(q.value(0).toString(), q.value(1).toString(),
                                q.value(2).toString());
        }
        reportResidentMeta->setText(meta);
    }
    updateCareTime();
}

// 날짜가 바뀌면 라벨을 고치고 그 날짜로 집계를 다시 돌린다.
void MainWindow::onReportDateChanged(const QDate& date)
{
    if (!date.isValid()) return;
    reportDate_ = date;

    if (reportDateLabel) {
        static const char* kDow[] = {"월", "화", "수", "목", "금", "토", "일"};
        const QString when = QStringLiteral("%1 (%2)")
                                 .arg(date.toString(QStringLiteral("yyyy-MM-dd")))
                                 .arg(QString::fromUtf8(kDow[date.dayOfWeek() - 1]));
        reportDateLabel->setText(date == QDate::currentDate()
                                     ? QStringLiteral("%1 · 오늘").arg(when)
                                     : when);
    }
    updateCareTime();
}

QWidget* MainWindow::buildSearchFilters()
{
    auto* bar = new QFrame();
    bar->setObjectName("filterBar");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);

    // 날짜/이벤트를 바꾸는 즉시 표에 필터가 적용된다 — 별도 '검색' 버튼은 없앴다.
    filterDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    filterDateFrom->setCalendarPopup(true);
    filterDateTo = new QDateEdit(QDate::currentDate());
    filterDateTo->setCalendarPopup(true);
    connect(filterDateFrom, &QDateEdit::dateChanged, this,
            [this](const QDate&) { applyLogFilters(true); });
    connect(filterDateTo, &QDateEdit::dateChanged, this,
            [this](const QDate&) { applyLogFilters(true); });

    filterRoom = new QComboBox();
    filterRoom->addItems({QStringLiteral("전체 병실"), QStringLiteral("201호-1"),
                          QStringLiteral("201호-2"), QStringLiteral("201호-3"),
                          QStringLiteral("201호-4")});

    filterEventType = new QComboBox();
    filterEventType->addItems({QStringLiteral("전체 이벤트"), QStringLiteral("낙상"),
                               QStringLiteral("침상이탈"), QStringLiteral("생체신호 이상")});
    // 드롭다운에서 항목을 고르는 즉시 표에 필터 적용(날짜 범위까지 함께)
    connect(filterEventType, &QComboBox::currentTextChanged,
            this, [this](const QString&) { applyLogFilters(true); });

    lay->addWidget(new QLabel(QStringLiteral("날짜")));
    lay->addWidget(filterDateFrom);
    lay->addWidget(new QLabel(QStringLiteral("~")));
    lay->addWidget(filterDateTo);
    lay->addWidget(new QLabel(QStringLiteral("병실")));
    lay->addWidget(filterRoom);
    lay->addWidget(new QLabel(QStringLiteral("이벤트")));
    lay->addWidget(filterEventType);
    lay->addStretch();
    return bar;
}

QWidget* MainWindow::buildLogTable()
{
    logTable = new QTableWidget(0, 4);
    logTable->setObjectName("logTable");
    logTable->setHorizontalHeaderLabels(
        {QStringLiteral("날짜/시간"), QStringLiteral("위치"),
         QStringLiteral("이벤트"), QStringLiteral("상태")});
    logTable->horizontalHeader()->setStretchLastSection(true);
    // 날짜/시간(0열)은 "yyyy-MM-dd HH:mm:ss"가 잘리지 않도록 내용 폭에 맞춘다.
    logTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    logTable->horizontalHeader()->setHighlightSections(false);
    logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    logTable->setSelectionMode(QAbstractItemView::SingleSelection);
    logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable->setShowGrid(false);                       // 격자선 대신 행 여백으로 구분
    logTable->setAlternatingRowColors(true);            // 얼룩 배경으로 행 가독성 ↑
    logTable->verticalHeader()->setVisible(false);      // 행 번호 숨김
    logTable->verticalHeader()->setDefaultSectionSize(38);  // 넉넉한 행 높이
    logTable->setCursor(Qt::PointingHandCursor);
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
        // playBlackboxClipAt()이 예약해둔 탐색 위치 — 길이가 확정된 지금이
        // setPosition을 걸 수 있는 시점(그 전엔 무시되거나 씹힐 수 있음).
        if (blackboxPendingSeekMs_ >= 0) {
            blackboxPlayer->setPosition(qBound<qint64>(0, blackboxPendingSeekMs_, dur));
            blackboxPendingSeekMs_ = -1;
        }
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

    if (clipDownloadButton) clipDownloadButton->setEnabled(true);
}

// blackboxUrl로 재생을 시작하되, 길이가 확정되면(durationChanged) 한 번 지정
// 위치로 탐색한다 — 이벤트↔NVR 연결에서 "그 시점 근처로 이동"할 때 사용.
void MainWindow::playBlackboxClipAt(const QString& url, qint64 seekMs)
{
    blackboxPendingSeekMs_ = qMax<qint64>(0, seekMs);
    playBlackboxClip(url);
}

// 로그에서 선택된 이벤트(selectedEventChannel_/selectedEventTimestampMs_)를
// 담고 있는 NVR 세그먼트를 nvrSegments_에서 찾아 그 시점으로 재생한다.
// 매칭 규칙: 같은 채널에서 시작시각이 이벤트 시각보다 앞선 것 중 가장 늦게
// 시작한 세그먼트(=그 시각을 담고 있을 세그먼트) — 세그먼트 길이는 클라가
// 몰라 정확한 종료시각 검증은 못 하지만, 서버 로테이션이 순차적이라 이걸로
// 충분하다.
void MainWindow::jumpToNvrContext()
{
    if (selectedEventChannel_ < 0 || selectedEventTimestampMs_ < 0) return;

    bool found = false;
    NvrSegmentInfo best;
    for (const auto& seg : nvrSegments_) {
        if (seg.channel != selectedEventChannel_) continue;
        if (seg.startMs > selectedEventTimestampMs_) continue;
        if (!found || seg.startMs > best.startMs) {
            best = seg;
            found = true;
        }
    }
    if (!found) {
        QMessageBox::information(this, QStringLiteral("NVR 없음"),
            QStringLiteral("이 시점에 해당하는 NVR 녹화를 찾을 수 없습니다.\n"
                           "(보존기간 경과, 미녹화 구간, 또는 아직 목록을 못 받아왔을 수 있습니다 — "
                           "새로고침 후 다시 시도해보세요.)"));
        return;
    }
    playBlackboxClipAt(best.url, selectedEventTimestampMs_ - best.startMs);
}

// 현재 재생기에 로드된 클립(블랙박스든 NVR이든 — blackboxUrl 하나로 공용)을
// 사용자가 고른 위치에 로컬 파일로 저장한다.
void MainWindow::downloadCurrentClip()
{
    if (blackboxUrl.isEmpty()) return;

    const QUrl srcUrl(blackboxUrl);
    const QString suggestedName = srcUrl.fileName();
    const QString savePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("클립 저장"), suggestedName,
        QStringLiteral("영상 파일 (*.mp4)"));
    if (savePath.isEmpty()) return;   // 사용자가 취소

    auto* manager = new QNetworkAccessManager(this);
    QNetworkReply* reply = manager->get(QNetworkRequest(srcUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, QStringLiteral("다운로드 실패"), reply->errorString());
            return;
        }
        QFile file(savePath);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, QStringLiteral("다운로드 실패"),
                QStringLiteral("파일을 저장할 수 없습니다: %1").arg(savePath));
            return;
        }
        file.write(reply->readAll());
        file.close();
        QMessageBox::information(this, QStringLiteral("다운로드 완료"),
            QStringLiteral("저장됨: %1").arg(savePath));
    });
}

// NVR(연속녹화) 탐색 — 채널 콤보 + 세그먼트 목록. 재생기는 새로 안 만들고
// 위의 블랙박스 재생기(blackboxPlayer 등)를 그대로 재사용한다.
QWidget* MainWindow::buildNvrBrowser()
{
    auto* card = new QFrame();
    card->setObjectName("videoCard");

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(6);

    auto* cap = new QLabel(QStringLiteral("NVR 연속녹화"));
    cap->setObjectName("panelTitle");
    lay->addWidget(cap);

    auto* topRow = new QHBoxLayout();
    nvrChannelCombo = new QComboBox();
    nvrChannelCombo->setObjectName("nvrChannelCombo");
    nvrChannelCombo->addItem(QStringLiteral("전체 채널"), -1);
    for (int ch = 0; ch < 4; ++ch)
        nvrChannelCombo->addItem(QStringLiteral("채널 %1").arg(ch + 1), ch);
    topRow->addWidget(nvrChannelCombo, 1);

    auto* refreshBtn = new QPushButton(QStringLiteral("새로고침"));
    refreshBtn->setObjectName("nvrRefreshBtn");
    refreshBtn->setCursor(Qt::PointingHandCursor);
    topRow->addWidget(refreshBtn);
    lay->addLayout(topRow);

    nvrSegmentList = new QListWidget();
    nvrSegmentList->setObjectName("nvrSegmentList");
    nvrSegmentList->setMaximumHeight(140);
    lay->addWidget(nvrSegmentList);

    connect(nvrChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::repopulateNvrList);
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshNvrSegments);
    connect(nvrSegmentList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
        if (!item) return;
        playBlackboxClip(item->data(Qt::UserRole).toString());
    });

    refreshNvrSegments();
    return card;
}

// 각 Pi의 NVR HTTP 서버(/list)에서 세그먼트 파일 목록을 받아 nvrSegments_에 누적하고
// 목록 UI를 다시 채운다. 파일명 규칙: ch{채널}_{세그먼트시작 unixMs}.mp4
void MainWindow::refreshNvrSegments()
{
    nvrSegments_.clear();
    for (int si = 0; si < kNumServers; ++si) {
        auto* manager = new QNetworkAccessManager(this);
        const QString host = serverHost(si);
        QUrl url(QStringLiteral("http://%1:%2/list").arg(host).arg(kNvrHttpPort));
        QNetworkReply* reply = manager->get(QNetworkRequest(url));

        connect(reply, &QNetworkReply::finished, this, [this, reply, host]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "⚠️ NVR 세그먼트 목록 수집 실패(" << host << "):" << reply->errorString();
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) return;

            for (const QJsonValue& value : doc.array()) {
                const QString fileName = value.toString();
                const QString cleanName = fileName.left(fileName.lastIndexOf('.'));
                const QStringList parts = cleanName.split(QLatin1Char('_'));
                if (parts.size() != 2) continue;   // ch{N}_{startMs} 형식만 (이벤트 클립과 구분)

                const int channel = parts[0].mid(2).toInt();  // "ch1" -> 1
                const qint64 startMs = parts[1].toLongLong();
                if (channel < 0 || channel >= 4) continue;

                NvrSegmentInfo entry;
                entry.channel = channel;
                entry.startMs = startMs;
                entry.url = QStringLiteral("http://%1:%2/%3")
                                .arg(host).arg(kNvrHttpPort).arg(fileName);
                nvrSegments_.push_back(entry);
            }
            repopulateNvrList();
        });
    }
}

// nvrChannelCombo 선택값 기준으로 nvrSegments_에서 골라 nvrSegmentList를 최신순으로 채움
void MainWindow::repopulateNvrList()
{
    if (!nvrSegmentList || !nvrChannelCombo) return;
    const int filterCh = nvrChannelCombo->currentData().toInt();

    QVector<NvrSegmentInfo> shown;
    for (const auto& seg : nvrSegments_) {
        if (filterCh >= 0 && seg.channel != filterCh) continue;
        shown.push_back(seg);
    }
    std::sort(shown.begin(), shown.end(),
              [](const NvrSegmentInfo& a, const NvrSegmentInfo& b) {
        return a.startMs > b.startMs;   // 최신순
    });

    nvrSegmentList->clear();
    for (const auto& seg : shown) {
        const QString when = QDateTime::fromMSecsSinceEpoch(seg.startMs)
                                  .toString("yyyy-MM-dd HH:mm:ss");
        auto* item = new QListWidgetItem(
            QStringLiteral("채널 %1 · %2").arg(seg.channel + 1).arg(when));
        item->setData(Qt::UserRole, seg.url);
        nvrSegmentList->addItem(item);
    }
}

// ═══════════════════════════════════════════════════════════
//  TAB3: DB 관리
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildDbTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    // ── 제목 + DB 상태 ──
    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("입소자 관리"));
    title->setObjectName("panelTitle");
    titleRow->addWidget(title);
    titleRow->addStretch();
    dbStatusDot = new QLabel();
    // 헤더 연결 상태등(7×7, #statusDot)과 objectName을 공유하면 그쪽의
    // border-radius:3px 규칙이 이 9×9 위젯에도 번진다 — 02-03 정정 B로 분리.
    dbStatusDot->setObjectName("dbStatusDot");
    dbStatusDot->setFixedSize(9, 9);
    // 사전 존재 결함(DYNAMIC-STYLE-INVENTORY.md #1, A-9): 이 점은 생성 시
    // 한 번 "정상" 색으로 칠해질 뿐, 실제 DB 연결 상태를 반영해 갱신하는
    // 호출부가 코드에 없다. 이 계획은 표시 메커니즘만 속성 기반으로 옮긴다
    // — 실제 DB 이벤트 배선은 새 기능이라 이 단계 요구사항 범위 밖이다.
    dbStatusDot->setProperty("severity", "normal");
    dbStatusDot->style()->unpolish(dbStatusDot);
    dbStatusDot->style()->polish(dbStatusDot);
    dbStatusDot->update();
    dbStatusText = new QLabel(QStringLiteral("DB 연결됨 · daboijo"));
    dbStatusText->setObjectName("statusText");
    titleRow->addWidget(dbStatusDot);
    titleRow->addWidget(dbStatusText);
    outer->addLayout(titleRow);

    // ── 상단 요약 통계 ──
    outer->addWidget(buildResidentSummary());

    // ── 본문: 좌 목록(마스터) | 우 상세/편집(디테일) ──
    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildResidentList(), 0);
    body->addWidget(buildResidentDetail(), 1);
    outer->addLayout(body, 1);

    return panel;
}

// 상단 요약 통계 바 — 재원 수 · 위험도 분포 · 채널 배정.
QWidget* MainWindow::buildResidentSummary()
{
    auto* host = new QWidget();
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);

    // category는 심각도가 아니라 통계 카테고리다 — resStatVal[category=...]
    // 규칙(base.qss)이 색을 결정한다. "재원" 카드는 이 전환으로 accent 텍스트
    // 색(FOUND-06 위반, 3:1 큰 글씨 기준조차 미달)에서 %(text)로 바뀐다 —
    // 의도된 시각 변화다.
    auto makeStat = [&](const QString& caption, const QString& category, QLabel*& ref) {
        auto* card = new QFrame();
        card->setObjectName("resStat");
        auto* v = new QVBoxLayout(card);
        v->setContentsMargins(16, 13, 16, 12);
        v->setSpacing(3);
        ref = new QLabel(QStringLiteral("0"));
        ref->setObjectName("resStatVal");
        ref->setProperty("category", category);
        auto* c = new QLabel(caption);
        c->setObjectName("resStatCap");
        v->addWidget(ref);
        v->addWidget(c);
        row->addWidget(card, 1);
    };

    makeStat(QStringLiteral("재원"),     QStringLiteral("active"),      resSumActive);
    makeStat(QStringLiteral("위험 상"),  QStringLiteral("danger-high"), resSumHigh);
    makeStat(QStringLiteral("위험 중"),  QStringLiteral("danger-mid"),  resSumMid);
    makeStat(QStringLiteral("위험 하"),  QStringLiteral("danger-low"),  resSumLow);
    makeStat(QStringLiteral("채널 배정"), QStringLiteral("channel"),    resSumCam);
    return host;
}

// 좌측 마스터 — 필터 세그먼트 + 검색 + 신규 + 세로 목록.
QWidget* MainWindow::buildResidentList()
{
    auto* wrap = new QFrame();
    wrap->setObjectName("listPanel");
    wrap->setFixedWidth(344);
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    // 재원 / 전체 / 퇴원 세그먼트
    auto* seg = new QHBoxLayout();
    seg->setSpacing(0);
    const QString modes[3] = {QStringLiteral("재원"), QStringLiteral("전체"),
                              QStringLiteral("퇴원")};
    for (int i = 0; i < 3; ++i) {
        residentFilterBtns[i] = new QPushButton(modes[i]);
        residentFilterBtns[i]->setObjectName("segTab");
        residentFilterBtns[i]->setCheckable(true);
        residentFilterBtns[i]->setChecked(i == 0);
        residentFilterBtns[i]->setCursor(Qt::PointingHandCursor);
        const QString m = modes[i];
        connect(residentFilterBtns[i], &QPushButton::clicked, this,
                [this, m] { setResidentFilter(m); });
        seg->addWidget(residentFilterBtns[i]);
    }
    v->addLayout(seg);

    // 검색창
    residentSearchEdit = new QLineEdit();
    residentSearchEdit->setObjectName("searchEdit");
    residentSearchEdit->setPlaceholderText(QStringLiteral("🔍  이름 검색"));
    residentSearchEdit->setClearButtonEnabled(true);
    residentSearchEdit->setFixedHeight(34);
    connect(residentSearchEdit, &QLineEdit::textChanged, this,
            [this](const QString& t) { refreshResidentCards(t); });
    v->addWidget(residentSearchEdit);

    // 신규 등록
    auto* newBtn = new QPushButton(QStringLiteral("＋ 신규 등록"));
    newBtn->setObjectName("primaryButton");
    newBtn->setCursor(Qt::PointingHandCursor);
    newBtn->setFixedHeight(34);
    connect(newBtn, &QPushButton::clicked, this, [this] { openResidentEditor(-1); });
    v->addWidget(newBtn);

    // 개수 캡션
    residentCountLabel = new QLabel(QStringLiteral("재원 목록"));
    residentCountLabel->setObjectName("segCaption");
    v->addWidget(residentCountLabel);

    // 세로 목록(스크롤)
    auto* scroll = new QScrollArea();
    scroll->setObjectName("vitalScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    residentCardHost = new QWidget();
    residentCardHost->setObjectName("cardHost");
    auto* listLay = new QVBoxLayout(residentCardHost);
    listLay->setContentsMargins(0, 0, 6, 0);
    listLay->setSpacing(8);
    scroll->setWidget(residentCardHost);
    v->addWidget(scroll, 1);
    return wrap;
}

// 재원/전체/퇴원 필터 전환 → 세그먼트 강조 + 목록 재조회.
void MainWindow::setResidentFilter(const QString& mode)
{
    residentFilter_ = mode;
    const QString modes[3] = {QStringLiteral("재원"), QStringLiteral("전체"),
                              QStringLiteral("퇴원")};
    for (int i = 0; i < 3; ++i)
        if (residentFilterBtns[i]) residentFilterBtns[i]->setChecked(modes[i] == mode);
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
}

// 우측 디테일 — 플레이스홀더/편집기를 스택으로 전환. (예전 팝업 다이얼로그 내용을 내장)
QWidget* MainWindow::buildResidentDetail()
{
    residentDetailStack = new QStackedWidget();

    // 0) 플레이스홀더
    auto* ph = new QFrame();
    ph->setObjectName("detailPanel");
    auto* pv = new QVBoxLayout(ph);
    pv->addStretch();
    auto* phLabel = new QLabel(
        QStringLiteral("왼쪽 목록에서 입소자를 선택하거나\n＋ 신규 등록을 눌러 시작하세요."));
    phLabel->setObjectName("detailPlaceholder");
    phLabel->setAlignment(Qt::AlignCenter);
    pv->addWidget(phLabel);
    pv->addStretch();
    residentDetailStack->addWidget(ph);

    // 1) 편집기 (프로필 헤더 + 폼|이력 + 액션바)
    auto* editor = new QFrame();
    editor->setObjectName("detailPanel");
    auto* root = new QVBoxLayout(editor);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // 상단 프로필 헤더
    auto* header = new QFrame();
    header->setObjectName("dlgHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 12, 16, 12);
    hl->setSpacing(14);
    dlgAvatar = new QLabel();
    dlgAvatar->setObjectName("dlgAvatar");
    dlgAvatar->setFixedSize(52, 52);
    dlgAvatar->setAlignment(Qt::AlignCenter);
    auto* nameCol = new QVBoxLayout();
    nameCol->setSpacing(2);
    dlgNameBig = new QLabel(QStringLiteral("신규 입소자"));
    dlgNameBig->setObjectName("dlgName");
    dlgSubMeta = new QLabel(QString());
    dlgSubMeta->setObjectName("dlgSub");
    nameCol->addWidget(dlgNameBig);
    nameCol->addWidget(dlgSubMeta);
    dlgRiskBadge = new QLabel();
    dlgRiskBadge->setObjectName("dlgRiskBadge");
    dlgStatusBadge = new QLabel();
    dlgStatusBadge->setObjectName("dlgStatusBadge");
    hl->addWidget(dlgAvatar);
    hl->addLayout(nameCol);
    hl->addStretch();
    hl->addWidget(dlgRiskBadge);
    hl->addWidget(dlgStatusBadge);
    root->addWidget(header);

    // 본문: 좌 폼(스크롤) | 우 입원이력
    auto* bodyLay = new QHBoxLayout();
    bodyLay->setSpacing(16);
    auto* formScroll = new QScrollArea();
    formScroll->setObjectName("vitalScroll");
    formScroll->setWidgetResizable(true);
    formScroll->setFrameShape(QFrame::NoFrame);
    formScroll->setWidget(buildResidentFormBody());
    bodyLay->addWidget(formScroll, 3);
    bodyLay->addWidget(buildAdmissionHistory(), 2);
    root->addLayout(bodyLay, 1);

    // 하단 액션바
    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    auto* saveBtn = new QPushButton(QStringLiteral("저장"));
    saveBtn->setObjectName("primaryButton");
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setMinimumWidth(96);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveResident);
    dlgDischargeBtn = new QPushButton(QStringLiteral("퇴원 처리"));
    dlgDischargeBtn->setObjectName("dangerButton");
    dlgDischargeBtn->setCursor(Qt::PointingHandCursor);
    connect(dlgDischargeBtn, &QPushButton::clicked, this, [this] {
        if (editStatus->currentText() == QStringLiteral("재원")) onDischargeResident();
        else                                                     onReadmitResident();
    });
    footer->addWidget(saveBtn);
    footer->addStretch();
    footer->addWidget(dlgDischargeBtn);
    root->addLayout(footer);

    residentDetailStack->addWidget(editor);
    residentDetailStack->setCurrentIndex(0);
    return residentDetailStack;
}

// 위험도/상태 텍스트를 색상 칩으로 만드는 헬퍼(파일 로컬).
// severity가 비어 있으면(예: "퇴원") 등급이 아니므로 도형을 붙이지 않고
// #riskChip 기본 규칙(중립색)에 맡긴다 — 상태 칩은 심각도가 아니다.
// 위젯이 아직 화면에 붙기 전에 만들어지므로(목록 재렌더마다 새로 생성) repolish
// 없이 속성만 설정해도 첫 표시 시 QSS가 적용된다 — #resRow의 inactive/selected
// 선례와 같다.
namespace {
QLabel* makeChip(const QString& text, const QString& severity) {
    const QString label = severity.isEmpty()
        ? text
        : severityGlyph(severity) + QStringLiteral(" ") + text;
    auto* chip = new QLabel(label);
    chip->setObjectName("riskChip");
    chip->setAttribute(Qt::WA_TransparentForMouseEvents);
    chip->setProperty("severity", severity);
    return chip;
}
}  // namespace

// 좌측 목록을 DB에서 다시 채운다. 현재 필터(재원/전체/퇴원) + 이름 검색을 함께 적용.
// 각 행 클릭 → 우측 인라인 편집기 표시. 선택된 행은 강조.
void MainWindow::refreshResidentCards(const QString& nameFilter)
{
    if (!residentCardHost || !residentCardHost->layout()) return;
    auto* box = qobject_cast<QVBoxLayout*>(residentCardHost->layout());
    if (!box) return;

    // 기존 행 제거(스트레치 포함)
    QLayoutItem* old;
    while ((old = box->takeAt(0))) {
        if (old->widget()) old->widget()->deleteLater();
        delete old;
    }

    const QString trimmed = nameFilter.trimmed();
    const bool searching = !trimmed.isEmpty();

    QString sql = QStringLiteral(
        "SELECT resident_id, name, room, bed, camera_id, wearable_id, risk_level, status "
        "FROM residents WHERE 1=1");
    if (residentFilter_ == QStringLiteral("재원"))      sql += QStringLiteral(" AND status='재원'");
    else if (residentFilter_ == QStringLiteral("퇴원")) sql += QStringLiteral(" AND status='퇴원'");
    if (searching) sql += QStringLiteral(" AND name LIKE ?");
    sql += QStringLiteral(" ORDER BY status DESC, camera_id, resident_id");

    QSqlQuery q;
    q.prepare(sql);
    if (searching) q.addBindValue(QStringLiteral("%%1%").arg(trimmed));
    if (!q.exec()) {
        qDebug() << "입소자 목록 조회 실패:" << q.lastError().text();
        return;
    }

    int n = 0;
    while (q.next()) {
        const int     id      = q.value(0).toInt();
        const QString name    = q.value(1).toString();
        const QVariant camVar  = q.value(4);
        const QString risk    = q.value(6).toString();
        const QString status  = q.value(7).toString();
        const bool    active  = (status == QStringLiteral("재원"));
        const QString riskSeverity = risk == QStringLiteral("상") ? QStringLiteral("critical")
                                   : risk == QStringLiteral("중") ? QStringLiteral("medium")
                                                                  : QStringLiteral("normal");

        // 행 = 클릭 가능한 버튼. 좌측에 위험도 색 띠(riskSeverity)로 위험도를 시각화.
        auto* rowBtn = new QPushButton();
        rowBtn->setObjectName("resRow");
        rowBtn->setProperty("inactive", !active);
        rowBtn->setProperty("selected", id == selectedResidentCardId);
        rowBtn->setCursor(Qt::PointingHandCursor);
        rowBtn->setFixedHeight(64);
        connect(rowBtn, &QPushButton::clicked, this, [this, id] { openResidentEditor(id); });

        auto* rl = new QHBoxLayout(rowBtn);
        rl->setContentsMargins(8, 8, 12, 8);
        rl->setSpacing(10);

        // 위험도 색 띠 — 글자가 없어 도형을 못 붙인다. 이 띠의 색 외 채널은
        // 같은 행의 위험도 칩(아래) 도형이다 — 이 짝짓기를 깨지 말 것(D-09).
        auto* riskBar = new QLabel();
        riskBar->setObjectName("riskBar");
        riskBar->setAttribute(Qt::WA_TransparentForMouseEvents);
        riskBar->setFixedWidth(4);
        riskBar->setProperty("severity", riskSeverity);
        rl->addWidget(riskBar);

        // 아바타 — active 속성으로 색을 받는다(재원 여부, 심각도 아님).
        // 이 위젯은 목록 재렌더마다 새로 만들어지고 아직 화면에 붙기 전이므로
        // 같은 행의 resRow와 동일하게 생성 직후 속성 설정만으로 충분하다.
        auto* avatar = new QLabel(name.left(1));
        avatar->setObjectName("resAvatar");
        avatar->setAttribute(Qt::WA_TransparentForMouseEvents);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setFixedSize(38, 38);
        avatar->setProperty("active", active);
        rl->addWidget(avatar);

        // 이름 + 채널
        auto* nameCol = new QVBoxLayout();
        nameCol->setSpacing(1);
        auto* nameLbl = new QLabel(name.isEmpty() ? QStringLiteral("(이름 없음)") : name);
        nameLbl->setObjectName("resName");
        nameLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QString chStr = camVar.isNull()
            ? QStringLiteral("채널 미지정")
            : QStringLiteral("채널 %1").arg(camVar.toInt() + 1);
        auto* metaLbl = new QLabel(chStr);
        metaLbl->setObjectName("resMeta");
        metaLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameCol->addWidget(nameLbl);
        nameCol->addWidget(metaLbl);
        rl->addLayout(nameCol, 1);

        // 우측: 위험도 칩(퇴원 행은 상태 칩 — 심각도가 아니므로 severity를 비운다)
        if (active)
            rl->addWidget(makeChip(QStringLiteral("위험 %1")
                                       .arg(risk.isEmpty() ? QStringLiteral("-") : risk),
                                   riskSeverity));
        else
            rl->addWidget(makeChip(QStringLiteral("퇴원"), QString()));

        box->addWidget(rowBtn);
        ++n;
    }
    box->addStretch(1);

    if (residentCountLabel) {
        const QString scope = searching ? QStringLiteral("검색 결과")
                                        : QStringLiteral("%1 목록").arg(residentFilter_);
        residentCountLabel->setText(QStringLiteral("%1 · %2명").arg(scope).arg(n));
    }
    refreshResidentSummary();
}

// 상단 요약 통계 — 재원 수 · 위험도(상/중/하) 분포 · 채널 배정 수.
void MainWindow::refreshResidentSummary()
{
    if (!resSumActive) return;
    int active = 0, high = 0, mid = 0, low = 0, cams = 0;

    QSqlQuery q;
    if (q.exec(QStringLiteral(
            "SELECT COUNT(*), "
            " SUM(risk_level='상'), SUM(risk_level='중'), SUM(risk_level='하'), "
            " COUNT(DISTINCT CASE WHEN camera_id BETWEEN 0 AND 3 THEN camera_id END) "
            "FROM residents WHERE status='재원'")) && q.next()) {
        active = q.value(0).toInt();
        high   = q.value(1).toInt();
        mid    = q.value(2).toInt();
        low    = q.value(3).toInt();
        cams   = q.value(4).toInt();
    }
    resSumActive->setText(QStringLiteral("%1명").arg(active));
    resSumHigh->setText(QString::number(high));
    resSumMid->setText(QString::number(mid));
    resSumLow->setText(QString::number(low));
    resSumCam->setText(QStringLiteral("%1/4").arg(cams));
}

// 편집 다이얼로그에 들어갈 폼(그룹들) — 버튼/스크롤은 다이얼로그 쪽에서 감싼다.
QWidget* MainWindow::buildResidentFormBody()
{
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
    basicForm->addRow(QStringLiteral("카메라 채널"), makeField("1~4", editCameraId));
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
    // 재원을 먼저 둬서 신규 등록 시 기본값이 재원이 되도록 한다.
    editStatus->addItems({QStringLiteral("재원"), QStringLiteral("퇴원")});
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
    lay->addStretch();

    return inner;
}

// 편집기 상단 프로필(아바타·이름·병상·배지·퇴원버튼)을 폼 현재값 기준으로 갱신.
void MainWindow::refreshResidentDialogHeader()
{
    if (!dlgNameBig) return;

    const bool isNew  = (selectedResidentId < 0);
    const QString name = editName->text().trimmed();
    const bool active  = (editStatus->currentText() == QStringLiteral("재원"));

    dlgAvatar->setText(name.isEmpty() ? QStringLiteral("＋") : name.left(1));
    // 목록 행 아바타와 달리 이 위젯은 편집 중 반복 갱신되므로 repolish가 필요하다.
    dlgAvatar->setProperty("active", active && !isNew);
    dlgAvatar->style()->unpolish(dlgAvatar);
    dlgAvatar->style()->polish(dlgAvatar);
    dlgAvatar->update();

    dlgNameBig->setText(isNew ? QStringLiteral("신규 입소자")
                              : (name.isEmpty() ? QStringLiteral("(이름 없음)") : name));

    const QString cam = editCameraId->text().trimmed();
    dlgSubMeta->setText(cam.isEmpty() ? QStringLiteral("채널 미지정")
                                      : QStringLiteral("채널 %1").arg(cam));

    // 이 함수는 콤보박스 값이 바뀔 때마다 다시 불리므로(생성 시 1회가 아니다)
    // 매번 repolish가 필요하다.
    const QString risk = editRiskLevel->currentText();
    const QString riskSeverity = risk == QStringLiteral("상") ? QStringLiteral("critical")
                                : risk == QStringLiteral("중") ? QStringLiteral("medium")
                                                                : QStringLiteral("normal");
    dlgRiskBadge->setText(severityGlyph(riskSeverity) + QStringLiteral(" ")
                           + QStringLiteral("위험 %1").arg(risk));
    dlgRiskBadge->setProperty("severity", riskSeverity);
    dlgRiskBadge->style()->unpolish(dlgRiskBadge);
    dlgRiskBadge->style()->polish(dlgRiskBadge);
    dlgRiskBadge->update();

    // 상태 배지(재원/퇴원)는 심각도가 아니다 — 재원 여부는 등급이 아니라
    // 분류다. severity를 쓰지 않고 별도 속성 residency로만 다루며 도형도
    // 붙이지 않는다.
    dlgStatusBadge->setText(active ? QStringLiteral("재원") : QStringLiteral("퇴원"));
    dlgStatusBadge->setProperty("residency", active ? "active" : "discharged");
    dlgStatusBadge->style()->unpolish(dlgStatusBadge);
    dlgStatusBadge->style()->polish(dlgStatusBadge);
    dlgStatusBadge->update();

    dlgRiskBadge->setVisible(!isNew);
    dlgStatusBadge->setVisible(!isNew);

    // 신규는 퇴원/재입원 불가 → 버튼 숨김. 기존은 상태에 따라 라벨 토글.
    dlgDischargeBtn->setVisible(!isNew);
    dlgDischargeBtn->setText(active ? QStringLiteral("퇴원 처리")
                                    : QStringLiteral("재입원"));
    // 신규 등록은 이력이 있을 수 없으므로 패널 자체를 숨겨 폼이 전체 폭을 쓰게 한다.
    if (admissionBox) admissionBox->setVisible(!isNew);
}

// 목록 행 클릭/신규 버튼 → 폼을 채우고 우측 인라인 편집기를 보여준다(팝업 없음).
void MainWindow::openResidentEditor(int residentId)
{
    if (residentId < 0) onNewResident();
    else                loadResidentIntoForm(residentId);
    refreshResidentDialogHeader();
    selectedResidentCardId = residentId;             // 목록에서 강조할 대상
    if (residentDetailStack) residentDetailStack->setCurrentIndex(1);
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
}

QWidget* MainWindow::buildAdmissionHistory()
{
    auto* box = new QGroupBox(QStringLiteral("입원 이력"));
    box->setObjectName("formGroup");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    admissionInfo = new QLabel(
        QStringLiteral("입소자를 선택하면 입원 이력이 표시됩니다. "
                       "행을 더블클릭하면 그 기간의 변경 내역이 열립니다."));
    admissionInfo->setObjectName("segCaption");
    admissionInfo->setWordWrap(true);
    lay->addWidget(admissionInfo);

    admissionTable = new QTableWidget(0, 4);
    admissionTable->setObjectName("logTable");
    admissionTable->setHorizontalHeaderLabels(
        {QStringLiteral("입원일"), QStringLiteral("퇴원일"),
         QStringLiteral("상태"),   QStringLiteral("변경")});
    admissionTable->horizontalHeader()->setStretchLastSection(true);
    admissionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    admissionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    admissionTable->verticalHeader()->setVisible(false);
    connect(admissionTable, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::onAdmissionRowActivated);
    lay->addWidget(admissionTable, 1);
    admissionBox = box;   // 신규 등록 시 숨기기 위해 보관
    return box;
}

void MainWindow::refreshAdmissionTable(int residentId)
{
    if (!admissionTable) return;
    admissionTable->setRowCount(0);
    if (residentId < 0) {
        admissionInfo->setText(QStringLiteral("입소자를 선택하세요."));
        return;
    }

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT a.admission_id, a.admitted_at, a.discharged_at, a.status, "
        " (SELECT COUNT(*) FROM resident_changes c "
        "   WHERE c.admission_id = a.admission_id) AS chg "
        "FROM admissions a WHERE a.resident_id=? "
        "ORDER BY a.admitted_at DESC, a.admission_id DESC"));
    q.addBindValue(residentId);
    if (!q.exec()) {
        qDebug() << "입원 이력 조회 실패:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        const int row = admissionTable->rowCount();
        admissionTable->insertRow(row);

        auto* inItem = new QTableWidgetItem(q.value(1).toDate().toString("yyyy-MM-dd"));
        inItem->setData(Qt::UserRole, q.value(0).toInt());   // admission_id 숨겨둠
        admissionTable->setItem(row, 0, inItem);

        const QVariant out = q.value(2);
        admissionTable->setItem(row, 1, new QTableWidgetItem(
                                            out.isNull() ? QStringLiteral("—") : out.toDate().toString("yyyy-MM-dd")));
        admissionTable->setItem(row, 2, new QTableWidgetItem(q.value(3).toString()));
        admissionTable->setItem(row, 3, new QTableWidgetItem(
                                            QStringLiteral("%1건").arg(q.value(4).toInt())));
    }

    admissionInfo->setText(QStringLiteral("입원 %1건 — 행 더블클릭 시 변경 내역")
                               .arg(admissionTable->rowCount()));
}

void MainWindow::onAdmissionRowActivated(int row, int /*column*/)
{
    auto* item = admissionTable ? admissionTable->item(row, 0) : nullptr;
    if (!item) return;
    showChangeLogDialog(item->data(Qt::UserRole).toInt());
}

// 선택한 입원 에피소드의 변경 내역만 팝업으로 — [수정 전] → [수정 후] 형식.
void MainWindow::showChangeLogDialog(int admissionId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT changed_at, change_type, field_label, old_value, new_value, changed_by "
        "FROM resident_changes WHERE admission_id=? "
        "ORDER BY changed_at DESC, change_id DESC"));
    q.addBindValue(admissionId);
    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("조회 실패"), q.lastError().text());
        return;
    }

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);   // 닫으면 자동 정리
    dlg->setObjectName("panel");
    dlg->setWindowTitle(QStringLiteral("변경 내역"));
    dlg->resize(820, 480);
    enableDarkTitleBar(dlg);

    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(10);

    auto* tbl = new QTableWidget(0, 5);
    tbl->setObjectName("logTable");
    tbl->setHorizontalHeaderLabels(
        {QStringLiteral("시각"), QStringLiteral("구분"), QStringLiteral("항목"),
         QStringLiteral("변경 내용"), QStringLiteral("작업자")});
    tbl->horizontalHeader()->setStretchLastSection(false);
    tbl->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->verticalHeader()->setVisible(false);

    auto shown = [](const QString& s) {
        return s.isEmpty() ? QStringLiteral("(없음)") : s;
    };

    while (q.next()) {
        const int row = tbl->rowCount();
        tbl->insertRow(row);
        tbl->setItem(row, 0, new QTableWidgetItem(
                                 q.value(0).toDateTime().toString("yyyy-MM-dd HH:mm:ss")));
        tbl->setItem(row, 1, new QTableWidgetItem(q.value(1).toString()));
        tbl->setItem(row, 2, new QTableWidgetItem(q.value(2).toString()));
        tbl->setItem(row, 3, new QTableWidgetItem(
                                 QStringLiteral("[수정 전] %1   →   [수정 후] %2")
                                     .arg(shown(q.value(3).toString()), shown(q.value(4).toString()))));
        tbl->setItem(row, 4, new QTableWidgetItem(q.value(5).toString()));
    }
    tbl->resizeColumnsToContents();
    tbl->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    if (tbl->rowCount() == 0) {
        auto* empty = new QLabel(QStringLiteral("이 입원 기간에 기록된 변경 내역이 없습니다."));
        empty->setObjectName("segCaption");
        lay->addWidget(empty);
    }
    lay->addWidget(tbl, 1);

    auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    closeBox->button(QDialogButtonBox::Close)->setText(QStringLiteral("닫기"));
    closeBox->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("roiButton"));
    connect(closeBox, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    lay->addWidget(closeBox);

    dlg->show();
}



// ═══════════════════════════════════════════════════════════
//  스타일 (QSS)
// ═══════════════════════════════════════════════════════════
void MainWindow::applyTheme()
{
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
        if (channelViews[ch] &&
            sockets[serverForChannel(ch)]->state() != QAbstractSocket::ConnectedState)
            channelViews[ch]->setLive(false);
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
//  웨어러블 바이탈 — MQTT(veda/wearable/data)로 들어온 값을 표시한다.
//
//  값이 없거나 오래된 채널은 "--" 로 둔다. 그럴듯한 숫자를 대신 띄우면
//  관제사가 멀쩡한 줄 알기 때문에, 모를 때는 모른다고 표시한다.
//
//  이 함수는 "표시만" 담당한다. 값 저장과 그래프 점 추가는 데이터가 실제로
//  도착한 순간(onWearableData)에 하고, 여기 타이머는 신호가 끊긴 걸 시간이
//  지나 알아채는 역할이다. (팔레트 전환 때도 색을 다시 입히려고 호출된다)
// ═══════════════════════════════════════════════════════════
void MainWindow::updateVitals()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 판정(무신호 3종 구분·등급·라벨·도형 접두)은 전부 여기서 한다 — VitalTile은
    // 완성된 표시값만 setLive()/setStale()로 받는다(D-02). 위젯 갱신의 4단계
    // repolish 관용구도 VitalTile 내부로 옮겨갔다(client/vitaltile.cpp).

    // 카드 단위로 돈다. 키는 입소자면 resident_id, 미배정 채널이면 음수 —
    // 음수 키는 vitals_ 에 값이 없어 기본값(received=false)이 잡히고 "대기"로 뜬다.
    for (auto it = vitalTiles_.constBegin(); it != vitalTiles_.constEnd(); ++it) {
        const int key = it.key();
        VitalTile* tile = it.value();
        if (!tile) continue;

        const VitalSample v = vitals_.value(key);
        const bool fresh = v.received && (now - v.arrivedAtMs) <= kVitalStaleMs;
        // 값은 오는데 전부 0 = 웨어러블 미착용. '신호 끊김'과도, '이상'과도 다른
        // 제3의 상태라 따로 표시한다(요양사 대응이 "기기를 채우세요"로 달라진다).
        const bool worn = vitalWorn(v.spo2, v.heartRate);

        if (!fresh || !worn) {
            // 세 가지를 구분한다 — 대응이 각각 다르다.
            //   대기      : 한 번도 안 옴 (등록/배선 문제)
            //   신호 끊김 : 오다가 멈춤 (기기 방전·중계 노드 다운)
            //   미착용    : 값은 오는데 전부 0 (기기를 안 차고 있음)
            // 도형(○)은 색 없이도 "이 카드는 심각도 등급이 아니라 신호가
            // 없는 상태"임을 알리는 채널 — 세 상태를 구분하는 건 텍스트다.
            const QString label = !fresh ? (v.received ? QStringLiteral("신호 끊김")
                                                        : QStringLiteral("대기"))
                                          : QStringLiteral("미착용");
            const QString badgeText = severityGlyph(QString(), /*stale=*/true)
                                       + QStringLiteral(" ") + label;
            // 커스텀 페인트 위젯(Sparkline)은 QSS를 못 받으므로 색 헬퍼를 거쳐
            // 중립색을 직접 받는다 — 전역 상수를 여기서 다시 읽지 않는다.
            tile->setStale(badgeText, severityColor(QString()));
            continue;
        }

        const int spo2 = v.spo2;
        const int hr   = v.heartRate;
        const QString severity = vitalSeverity(spo2, hr);
        // 상태 배지에만 등급 도형을 접두한다 — SpO2/심박 값 라벨엔 붙이지
        // 않는다(숫자 판독 방해 + 배지가 이미 도형을 들고 있어 중복 부호화는
        // 이미 충족된다).
        const QString status = vitalStatusLabel(spo2, hr);
        const QString badgeText = severityGlyph(severity) + QStringLiteral(" ") + status;
        tile->setLive(spo2, hr, severity, badgeText, severityColor(severity));
    }
}

// ═══════════════════════════════════════════════════════════
//  MQTT 수신 — 웨어러블 생체·낙상 (veda/wearable/data)
// ═══════════════════════════════════════════════════════════
void MainWindow::onWearableData(const WearableData& data)
{
    // 브로커는 기기 id 로만 알려준다. 그 기기가 누구 것인지로 건너간다.
    // ★ 채널이 아니라 입소자로 잇는다 — 한 채널에 여러 명이 있으면 채널로는
    //   누구 값인지 구분이 안 돼 나중에 온 값이 앞 값을 덮어쓴다.
    // 등록되지 않은 기기는 누구 것인지 알 수 없어 버린다 — 엉뚱한 사람 자리에
    // 남의 심박수를 띄우느니 안 띄우는 편이 낫다.
    const QString id = QString::fromStdString(data.device_id).trimmed();
    const auto it = wearableToResident.constFind(id);
    if (it == wearableToResident.constEnd()) {
        qDebug() << "[MQTT] 미등록 웨어러블 무시:" << id
                 << "(residents.wearable_id 에 등록하면 해당 입소자에게 표시됩니다)";
        return;
    }

    const int rid = it.value();

    VitalSample& v = vitals_[rid];
    v.received    = true;
    v.heartRate   = data.heart_rate;
    v.spo2        = data.spo2;
    v.arrivedAtMs = QDateTime::currentMSecsSinceEpoch();

    // 그래프 점은 값이 실제로 도착했을 때만 찍는다.
    // 위젯 밖(hrHistory_)에도 같이 쌓아둬야 카드를 다시 만들 때 추세가 살아난다.
    QVector<double>& hist = hrHistory_[rid];
    hist.append(data.heart_rate);
    while (hist.size() > kHrHistoryMax) hist.removeFirst();
    if (VitalTile* tile = vitalTiles_.value(rid)) tile->pushHeartRateSample(data.heart_rate);

    // 웨어러블이 낙상을 감지한 경우. 카메라 낙상(TCP 0xDB4D)과는 별개 경로라
    // 같은 사건이 두 번 들어올 수 있다 — 이미 경보 중인 채널은 다시 울리지 않는다.
    // ★ 웨어러블은 기기가 사람마다 달라서 "누가" 넘어졌는지까지 알 수 있다.
    //   (카메라 낙상은 채널까지만 알 수 있어 이름을 붙일 수 없다)
    const ResidentInfo info = residentInfo_.value(rid);
    if (data.is_fall_detected && info.channel >= 0 && info.channel < 4
        && !fallActive[info.channel]) {
        qDebug() << "[MQTT] 웨어러블 낙상 감지 —" << info.name
                 << "채널" << (info.channel + 1) << "기기" << id;

        // 카메라 낙상(handleFallEvent)과 같은 상태로 만든다. 다만 비상 로그에는
        // 넣지 않는다 — 그쪽 행은 블랙박스 클립 URL 을 달고 있는데, 웨어러블만
        // 감지한 낙상은 녹화된 클립이 없어서 눌러도 열리지 않는 행이 된다.
        fallActive[info.channel] = true;
        if (channelViews[info.channel]) {
            // 웨어러블은 기기가 사람마다 달라 "누가" 넘어졌는지까지 띄울 수 있다.
            channelViews[info.channel]->setAlert(
                true, QStringLiteral("🚨 %1 낙상 감지").arg(info.name));
        }
        refreshAlarmButton();          // → updateAlarmBanner() 로 상단 배너가 내려온다
        setVideoFocus(info.channel);   // 감지 채널을 크게(스포트라이트)
    }

    updateVitals();   // 도착 즉시 화면 반영 (2초 타이머를 기다리지 않는다)
}

// ═══════════════════════════════════════════════════════════
//  MQTT 수신 — 알림 노드로 나간 제어 명령 (veda/alarm/control)
//  우리가 보낸 것도 되돌아오므로, 로그에 쌓을 때는 걸러야 한다.
// ═══════════════════════════════════════════════════════════
void MainWindow::onMqttAlarm(const AlarmCommand& cmd)
{
    qDebug() << "[MQTT] 알림 명령:"
             << QString::fromStdString(cmd.type)
             << cmd.room                             // AlarmCommand::room 은 정수(호실 번호)
             << QString::fromStdString(cmd.message);
}

void MainWindow::onMqttConnected()
{
    qInfo() << "[MQTT] 브로커 연결됨 —" << brokerHost() << ":" << brokerPort();
}

void MainWindow::onMqttDisconnected()
{
    // 값이 끊긴 건 updateVitals 가 30초 뒤 "신호 끊김" 으로 알려준다.
    qWarning() << "[MQTT] 브로커 연결 끊김";
}

void MainWindow::onMqttError(const QString& message)
{
    qWarning() << "[MQTT]" << message;
}

void MainWindow::onAlarmNodeStatus(const QString& node, bool online)
{
    alertNodeOnline_[node] = online;
    if (alertNode_ && alertNode_->currentText() == node)
        refreshAlertStatusBadge();
}

// 배지 3상태 — severity 속성 + #alertNodeBadge QSS 규칙.
//   미확인(도형 ○, severity 없음) / 온라인(✓, normal) / 오프라인(✖, critical)
void MainWindow::refreshAlertStatusBadge()
{
    if (!alertStatusBadge_ || !alertNode_) return;
    const QString node = alertNode_->currentText();
    QString text, severity;
    if (!alertNodeOnline_.contains(node)) {
        text = QStringLiteral("상태 미확인");
        severity = QString();  // 심각도 어느 값에도 걸리지 않는 중립 상태
    } else if (alertNodeOnline_.value(node)) {
        text = QStringLiteral("온라인");
        severity = QStringLiteral("normal");
    } else {
        text = QStringLiteral("오프라인");
        severity = QStringLiteral("critical");
    }
    alertStatusBadge_->setText(severityGlyph(severity, severity.isEmpty())
                                + QStringLiteral(" ") + text);
    alertStatusBadge_->setProperty("severity", severity);
    alertStatusBadge_->style()->unpolish(alertStatusBadge_);
    alertStatusBadge_->style()->polish(alertStatusBadge_);
    alertStatusBadge_->update();
}

// ═══════════════════════════════════════════════════════════
//  일일 리포트 지표 — 선택한 날짜 + 선택한 입소자의 4개 값을 채운다.
//  원천은 전부 서버가 쌓는다: care_logs / bed_sessions / activity_minute / events
// ═══════════════════════════════════════════════════════════
namespace {
// 초 → "8시간 20분 30초" / "4분 27초" / "35초". 0이면 "—"로 비운다.
//
// ★ 초를 버리지 않는다. 케어 세션은 수십 초짜리가 흔해서 분 단위로 자르면
//   "4분 27초"가 "4분"이 되고, 하루치를 합산할 때 사람마다 최대 59초씩 사라진다.
//   대신 값이 0인 자리는 빼서 "1시간 0분 30초" 같은 군더더기는 안 나오게 한다.
QString humanDuration(int sec)
{
    if (sec <= 0) return QStringLiteral("—");

    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;

    QStringList parts;
    if (h > 0) parts << QStringLiteral("%1시간").arg(h);
    if (m > 0) parts << QStringLiteral("%1분").arg(m);
    if (s > 0) parts << QStringLiteral("%1초").arg(s);
    return parts.join(QLatin1Char(' '));
}
}  // namespace

void MainWindow::updateCareTime()
{
    const int rid = reportResidentId_;
    if (!tileCareVal) return;              // 아직 화면이 만들어지기 전

    // 입소자가 없으면(전원 퇴원 등) 전부 비운다 — 남의 숫자가 남아 있으면 안 된다.
    if (rid <= 0) {
        for (QLabel* l : {tileLyingVal, tileActivityVal, tileCareVal, tileEventVal})
            if (l) l->setText(QStringLiteral("—"));
        for (QLabel* l : {tileLyingSub, tileActivitySub, tileCareSub, tileEventSub})
            if (l) l->setText(QStringLiteral(" "));
        if (activityChart) activityChart->clear();
        return;
    }

    const QDateTime dayStart(reportDate_, QTime(0, 0));
    const QDateTime dayEnd = dayStart.addDays(1);

    // ── 케어시간 ──
    // ★ 하루 구분은 end_time 이 아니라 start_time 기준이다. 서버가 요양사의 잠깐
    //   자리 비움 후 복귀를 직전 행에 합산하면서 end_time 을 뒤로 미는데,
    //   end_time 으로 자르면 자정 직전에 시작한 케어가 통째로 다음 날로 넘어간다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(duration_sec),0), COUNT(*), MAX(end_time) "
            "FROM care_logs WHERE resident_id=? AND DATE(start_time)=?"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec() && q.next()) {
            tileCareVal->setText(humanDuration(q.value(0).toInt()));
            const QDateTime last = q.value(2).toDateTime();
            tileCareSub->setText(
                q.value(1).toInt() == 0
                    ? QStringLiteral("기록 없음")
                    : QStringLiteral("%1회 · 마지막 %2")
                          .arg(q.value(1).toInt())
                          .arg(last.isValid() ? last.toString(QStringLiteral("HH:mm"))
                                              : QStringLiteral("—")));
        } else {
            qDebug() << "케어로그 조회 실패:" << q.lastError().text();
        }
    }

    // ── 누워있는 시간 ──
    // ★ 재실 세션은 자정을 넘기는 게 정상이다(23시 취침 → 익일 7시 기상). "시작한
    //   날"로 몰면 안 되고 이 날짜 구간과의 교집합만 세야 한다. 그래서 GREATEST/LEAST
    //   로 양끝을 자른다. 아직 안 닫힌 세션(out_at IS NULL)은 지금 시각까지로 본다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(TIMESTAMPDIFF(SECOND, GREATEST(in_at, ?), "
            "                                  LEAST(COALESCE(out_at, NOW()), ?))), 0), "
            "       COUNT(*) "
            "FROM bed_sessions WHERE resident_id=? "
            "  AND in_at < ? AND COALESCE(out_at, NOW()) > ?"));
        q.addBindValue(dayStart);
        q.addBindValue(dayEnd);
        q.addBindValue(rid);
        q.addBindValue(dayEnd);
        q.addBindValue(dayStart);
        if (q.exec() && q.next()) {
            tileLyingVal->setText(humanDuration(q.value(0).toInt()));
            const int n = q.value(1).toInt();
            tileLyingSub->setText(n == 0 ? QStringLiteral("기록 없음")
                                         : QStringLiteral("재실 %1회").arg(n));
        } else {
            qDebug() << "재실 세션 조회 실패:" << q.lastError().text();
        }
    }

    // ── 활동량(만보기) ──
    // 걸음 수는 누적값이라 서버가 1분치 증가분(steps_delta)으로 눌러 담는다.
    // "활동한 분"은 그 분에 10걸음 이상 늘어난 분의 개수다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(steps_delta),0), COALESCE(SUM(steps_delta>=10),0) "
            "FROM activity_minute WHERE resident_id=? AND DATE(minute_ts)=?"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec() && q.next()) {
            const int steps = q.value(0).toInt();
            const int activeMin = q.value(1).toInt();
            tileActivityVal->setText(steps > 0 ? QStringLiteral("%1걸음").arg(steps)
                                               : QStringLiteral("—"));
            tileActivitySub->setText(steps > 0 ? QStringLiteral("활동 %1분").arg(activeMin)
                                               : QStringLiteral("기록 없음"));
        } else {
            qDebug() << "활동량 조회 실패:" << q.lastError().text();
        }
    }

    // ── 24시간 활동량 그래프 ──
    // 분 단위로 쌓아둔 걸 시간으로 묶어서 24칸을 만든다. 심박은 합계가 아니라
    // 평균이고, 측정 실패(0)는 평균에서 빼야 값이 통째로 내려앉지 않는다.
    if (activityChart) {
        QVector<int> stepsByHour(24, 0), hrByHour(24, 0);
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT HOUR(minute_ts), COALESCE(SUM(steps_delta),0), "
            "       ROUND(AVG(NULLIF(hr_avg,0))) "
            "FROM activity_minute WHERE resident_id=? AND DATE(minute_ts)=? "
            "GROUP BY HOUR(minute_ts)"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const int h = q.value(0).toInt();
                if (h < 0 || h > 23) continue;
                stepsByHour[h] = q.value(1).toInt();
                hrByHour[h]    = q.value(2).toInt();   // NULL 이면 0 → 선이 끊긴다
            }
        } else {
            qDebug() << "시간별 활동량 조회 실패:" << q.lastError().text();
        }
        activityChart->setData(stepsByHour, hrByHour);
    }

    // ── 이벤트 횟수 ──
    // 종류별로 세서 "3회 (낙상1·이탈2)"처럼 내역까지 보여준다. 숫자만 있으면
    // 무슨 일이 있었는지 알 수 없어 리포트로 쓸모가 떨어진다.
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT event_type, COUNT(*) FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? GROUP BY event_type"));
        q.addBindValue(rid);
        q.addBindValue(reportDate_);
        int total = 0;
        QStringList parts;
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(0).toString();
                const int n = q.value(1).toInt();
                total += n;
                const QString label = t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                                    : t == QLatin1String("EGRESS") ? QStringLiteral("이탈")
                                                                   : QStringLiteral("생체");
                parts << QStringLiteral("%1%2").arg(label).arg(n);
            }
        } else {
            qDebug() << "이벤트 조회 실패:" << q.lastError().text();
        }
        tileEventVal->setText(total > 0 ? QStringLiteral("%1회").arg(total)
                                        : QStringLiteral("—"));
        tileEventSub->setText(parts.isEmpty() ? QStringLiteral("이벤트 없음")
                                              : parts.join(QStringLiteral(" · ")));
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
                // roi_id = 서버가 밝힌 발생 침대(=누구). 못 밝혔으면 kRoiIdNone.
                const int roiId = (evt.roi_id < kMaxRoiZones) ? int(evt.roi_id) : -1;
                if (evt.type == kEvtFall) {
                    handleFallEvent(evt.channel, roiId, evt.timestamp_ms,
                                    evt.x / float(kRoiCoordScale),
                                    evt.y / float(kRoiCoordScale));
                }
                else if (evt.type == kEvtBedEgress) {
                    handleBedEgressEvent(evt.channel, roiId, evt.timestamp_ms,
                                         evt.x / float(kRoiCoordScale),
                                         evt.y / float(kRoiCoordScale));
                }
                // 웨어러블 생체데이터 이상 — x,y 는 쓰지 않는다(서버가 0 으로 채움)
                else if (evt.type == kEvtVitalAbnormal) {
                    handleVitalAbnormalEvent(evt.channel, evt.timestamp_ms);
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
        // 해제한 채널은 서버가 잠깐 더 보내는 프레임을 무시 → 검은 미연결 화면 유지
        if (videoSuppressed_[ch])
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

        const QPixmap pix = QPixmap::fromImage(image);
        lastFramePix_[ch] = pix;           // 이미지 탭 Before/After 프리뷰용 최신본 보관
        channelViews[ch]->setFrame(pix);
        channelViews[ch]->setLive(true);   // 프레임 도착 → LIVE 표시등 점등
        // ROI 편집기가 이 채널을 보고 있고 설정 탭이 열려 있으면 편집 영상도 실시간 갱신.
        if (roiEditorView && roiEditChannel == ch && cameraSettingsVisible()) {
            roiEditorView->setFrame(pix);
            roiEditorView->setLive(true);   // 프레임이 오는 중 → LIVE 점등
        }
        // 좌측 채널 스트립 썸네일 — 작게 그리므로 비용은 출력 픽셀 수에 비례해 미미하다.
        if (camThumbs[ch] && cameraSettingsVisible())
            camThumbs[ch]->setFrame(pix);
        // 이미지 탭 '적용 후(실시간)' 프리뷰도 같은 프레임으로 즉시 갱신 — 라이브 영상과
        // 동일 경로라 지연이 붙지 않는다. (이미지 모드일 때만: 나머지 모드에선 안 보임)
        if (imgAfter && roiEditChannel == ch && cameraSettingsVisible() &&
            camMode_ == QStringLiteral("이미지"))
            imgAfter->setFrame(pix);
    }
}

// ═══════════════════════════════════════════════════════════
//  낙상 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleFallEvent(int channel, int roiId, quint64 timestampMs,
                                 float nx, float ny)
{
    // 서버가 이 사람을 어느 침대 입소자로 귀속했는지 — 못 밝혔으면 roiId<0.
    // 낙상자는 침대 밖이라 위치로는 알 수 없고, 서버 IdentityTracker의 추정이다.
    const QString who = eventWhoLabel(channel, roiId);

    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        fallActive[channel] = true;
        if (channelViews[channel]) {
            // 서버가 보낸 낙상 발생 위치(정규화 0~1)에 십자 조준점 표시
            channelViews[channel]->setAlert(
                true, QStringLiteral("🚨 낙상 감지 · %1").arg(who), QPointF(nx, ny));
        }
        qDebug() << "🚨 [낙상 감지] 채널" << (channel + 1) << who
                 << "빨간 테두리 켜짐 (모자이크 자동 해제 상태)";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)

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
        dtItem->setData(Qt::UserRole + 1, channel);
        dtItem->setData(Qt::UserRole + 2, static_cast<qint64>(timestampMs));
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(eventPlaceLabel(channel, roiId)));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("낙상")));
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));

        logTable->setSortingEnabled(true);
        logTable->sortItems(0, Qt::DescendingOrder);   // 최신 이벤트가 위로
        applyLogFilters();   // 현재 필터 조건을 새로 들어온 행에도 적용
    }
}

// ═══════════════════════════════════════════════════════════
//  침상 이탈 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleBedEgressEvent(int channel, int roiId, quint64 timestampMs,
                                      float nx, float ny)
{
    // 이탈은 "어느 침대에서 나갔는가"가 판정 자체라, 낙상과 달리 추정이 아니라
    // 확정된 침대다. 그 침대에 매핑된 입소자가 곧 누구인지.
    const QString who = eventWhoLabel(channel, roiId);

    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        bedEgressActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(
                true, QStringLiteral("⚠️ 침대 이탈 · %1").arg(who),
                (nx > 0 || ny > 0) ? QPointF(nx, ny) : QPointF(-1, -1));
        }
        qDebug() << "⚠️ [침상 이탈 감지] 채널" << (channel + 1) << who << "빨간 테두리 켜짐";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)

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
        dtItem->setData(Qt::UserRole + 1, channel);
        dtItem->setData(Qt::UserRole + 2, static_cast<qint64>(timestampMs));
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(eventPlaceLabel(channel, roiId)));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("침상 이탈"))); // 💡 이탈 분류로 등록
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));

        logTable->setSortingEnabled(true);
        logTable->sortItems(0, Qt::DescendingOrder);   // 최신 이벤트가 위로
    }
}

// ═══════════════════════════════════════════════════════════
//  웨어러블 생체신호 이상 — 빨간색 테두리 활성화 및 로그 추가
//
//  카메라가 아니라 웨어러블이 근거인 경보다. 발생 위치라는 개념이 없어
//  십자 조준점을 찍지 않는 것만 낙상/이탈과 다르고, 나머지 흐름은 같다.
// ═══════════════════════════════════════════════════════════
void MainWindow::handleVitalAbnormalEvent(int channel, quint64 timestampMs)
{
    // 1. 빨간 테두리 즉각 활성화
    if (channel >= 0 && channel < 4) {
        vitalAbnormalActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(true, QStringLiteral("🚨 생체신호 이상"));
        }
        qDebug() << "🚨 [생체신호 이상] 채널" << (channel + 1) << "빨간 테두리 켜짐";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable) {
        logTable->setSortingEnabled(false);   // 삽입 중 재정렬 방지

        const int row = logTable->rowCount();
        logTable->insertRow(row);

        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 static_cast<qint64>(timestampMs)).toString("yyyy-MM-dd HH:mm:ss");
        auto* dtItem = new QTableWidgetItem(when);

        // 서버 저장 규칙: blackbox.trigger(ch, "VITAL_ABNORMAL") 과 접미사를 맞춘다
        const QString clipUrl = QStringLiteral("http://%1:%2/ch%3_%4_VITAL_ABNORMAL.mp4")
                                     .arg(hostForChannel(channel))
                                     .arg(kClipHttpPort)
                                     .arg(channel)
                                     .arg(timestampMs);
        dtItem->setData(Qt::UserRole, clipUrl);
        dtItem->setData(Qt::UserRole + 1, channel);
        dtItem->setData(Qt::UserRole + 2, static_cast<qint64>(timestampMs));
        logTable->setItem(row, 0, dtItem);
        logTable->setItem(row, 1, new QTableWidgetItem(patients[channel].bed));
        logTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("생체신호 이상")));
        logTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("미확인")));

        logTable->setSortingEnabled(true);
        logTable->sortItems(0, Qt::DescendingOrder);   // 최신 이벤트가 위로
        applyLogFilters();   // 현재 필터 조건을 새로 들어온 행에도 적용
    }
}

// ═══════════════════════════════════════════════════════════
//  침대 ROI 지정 / 표시 / 서버 전송
// ═══════════════════════════════════════════════════════════
void MainWindow::onRoiButtonClicked()
{
    if (!roiEditorView) return;
    // 그리는 중이면 이 버튼은 "취소"로 동작
    if (roiEditorView->drawMode()) {
        roiEditorView->cancelDraft();
        return;
    }
    // 침대가 상한까지 찼으면 그리기를 시작하지 않는다 — 그려 놓고 "못 넣는다"고
    // 하면 그린 노동이 그대로 버려진다.
    if (roiEditorView->nextFreeZoneId() < 0) {
        QMessageBox::information(
            this, QStringLiteral("침대 추가"),
            QStringLiteral("채널 %1에는 침대를 최대 %2개까지 지정할 수 있습니다.\n"
                           "기존 침대를 제거한 뒤 다시 시도하세요.")
                .arg(roiEditChannel + 1)
                .arg(kMaxRoiZones));
        return;
    }
    // 편집기(현재 선택 채널)에 바로 그리기 시작 (좌클릭=점, 더블클릭=완료)
    roiEditorView->setDrawMode(true);
}

// "침대 제거" — 선택된 침대 1개, 선택이 없으면 채널 전체.
void MainWindow::onRoiClearClicked()
{
    const int ch = roiEditChannel;
    if (roiEditorView && roiEditorView->drawMode()) roiEditorView->cancelDraft();

    if (roiZones_[ch].isEmpty()) {
        QMessageBox::information(this, QStringLiteral("침대 제거"),
                                 QStringLiteral("채널 %1에 제거할 침대가 없습니다.").arg(ch + 1));
        return;
    }

    const int sel = roiEditorView ? roiEditorView->selectedZone() : -1;
    const bool all = (sel < 0);
    const QString what = all
        ? QStringLiteral("채널 %1의 침대 %2개를 모두 제거할까요?")
              .arg(ch + 1).arg(roiZones_[ch].size())
        : QStringLiteral("채널 %1의 침대 %2(%3)를 제거할까요?")
              .arg(ch + 1).arg(sel + 1).arg(zoneResidentName(ch, sel));

    if (QMessageBox::question(this, QStringLiteral("침대 제거"), what) != QMessageBox::Yes)
        return;

    // 서버에 삭제 통보 — roi_id에 kRoiIdAll을 실으면 그 채널 전체 삭제다.
    sendRoi(ch, all ? kRoiIdAll : sel, QPolygonF(), true);

    if (all) {
        roiZones_[ch].clear();
        roiResident_[ch].clear();
    } else {
        for (int i = 0; i < roiZones_[ch].size(); ++i) {
            if (roiZones_[ch][i].id != sel) continue;
            roiZones_[ch].removeAt(i);
            break;
        }
        roiResident_[ch].remove(sel);
    }
    refreshRoiZones(ch);
    qDebug() << "침대 제거: ch" << ch << (all ? -1 : sel);
}

// 침대 ROI를 DB(roi_zones)에서 읽어 화면에 복원한다.
// 서버도 같은 표를 읽어 판정을 복원하므로(server/src/main.cpp), Qt를 껐다 켜도
// 관제 화면과 서버가 같은 침대를 본다. 그리기 결과를 Qt 메모리에만 두면
// 재시작 후 화면엔 침대가 없는데 서버는 판정 중인 상태가 되어 서로 어긋난다.
void MainWindow::loadRoiZonesFromDb()
{
    for (int ch = 0; ch < 4; ++ch) {
        roiZones_[ch].clear();
        roiResident_[ch].clear();
    }

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT camera_id, roi_id, resident_id, roi_points FROM roi_zones "
            "ORDER BY camera_id, roi_id"))) {
        qDebug() << "침대 ROI 조회 실패:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        const int ch = q.value(0).toInt();
        const int id = q.value(1).toInt();
        if (ch < 0 || ch >= 4 || id < 0 || id >= kMaxRoiZones) continue;

        const int rid = q.value(2).isNull() ? 0 : q.value(2).toInt();
        if (rid > 0) roiResident_[ch][id] = rid;

        // roi_points는 서버가 쓴 [[x,y],...] JSON. 숫자만 순서대로 긁어 둘씩 묶는다
        // (이 컬럼에 다른 구조가 들어올 경로가 없다 — 쓰는 쪽이 서버 한 곳뿐).
        const QString json = q.value(3).toString();
        QVector<double> nums;
        for (int i = 0; i < json.size();) {
            const QChar c = json[i];
            if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('.')) {
                int j = i;
                while (j < json.size() &&
                       (json[j].isDigit() || json[j] == QLatin1Char('-') ||
                        json[j] == QLatin1Char('.') || json[j] == QLatin1Char('e')))
                    ++j;
                nums.push_back(json.mid(i, j - i).toDouble());
                i = j;
            } else {
                ++i;
            }
        }
        RoiZone z;
        z.id = id;
        for (int i = 0; i + 1 < nums.size(); i += 2)
            z.poly << QPointF(nums[i], nums[i + 1]);
        if (z.poly.size() >= 3) roiZones_[ch].push_back(z);
    }

    for (int ch = 0; ch < 4; ++ch) refreshRoiZones(ch);
    qDebug() << "침대 ROI 복원 완료";
}

// 이벤트 배너에 쓸 "어디서 / 누가" 문구.
//
// 서버가 침대를 특정하지 못하면(roiId<0) 사람 대신 채널만 말한다.
// ★ "신원 미상"이라고 쓰지 않는다 — 요양원에서 그 표현은 "모르는 사람이 들어왔다"
//   (외부인 침입)로 읽힌다. 실제 의미는 "우리가 누군지 못 밝혔다"일 뿐이라
//   알 수 있는 사실(어느 채널)만 말하는 쪽이 정확하고 오해도 없다.
// ★ 반대로 추정한 이름을 억지로 붙이지도 않는다. 추적 ID는 신원이 아니라서
//   오귀속이 원리적으로 가능하고, 틀린 이름은 이름 없는 것보다 나쁘다
//   (엉뚱한 보호자에게 연락이 간다).
QString MainWindow::eventWhoLabel(int channel, int roiId) const
{
    if (roiId < 0) return QStringLiteral("채널 %1").arg(channel + 1);
    const QString name = zoneResidentName(channel, roiId);
    return name == QStringLiteral("미지정")
               ? QStringLiteral("침대 %1").arg(roiId + 1)
               : QStringLiteral("침대 %1 %2").arg(roiId + 1).arg(name);
}

// 이벤트 로그 '위치' 칸 문구 — 침대를 특정했으면 침대까지, 아니면 채널까지.
QString MainWindow::eventPlaceLabel(int channel, int roiId) const
{
    if (channel < 0 || channel >= 4) return QString();
    if (roiId < 0) return patients[channel].bed;
    return QStringLiteral("%1 · 침대 %2").arg(patients[channel].bed).arg(roiId + 1);
}

// 이 침대에 매핑된 입소자 이름. 미지정이면 "미지정".
QString MainWindow::zoneResidentName(int channel, int roiId) const
{
    if (channel < 0 || channel >= 4) return QStringLiteral("미지정");
    const int rid = roiResident_[channel].value(roiId, 0);
    if (rid <= 0) return QStringLiteral("미지정");
    auto it = residentInfo_.constFind(rid);
    return it == residentInfo_.constEnd() || it->name.isEmpty()
               ? QStringLiteral("입소자 %1").arg(rid)
               : it->name;
}

// 침대 목록(roiZones_/roiResident_)을 화면 전체에 다시 반영한다.
// 오버레이 이름표·4분할 화면·인스펙터 목록·채널 배지가 한 소스를 보게 하는 지점.
void MainWindow::refreshRoiZones(int channel)
{
    if (channel < 0 || channel >= 4) return;

    // 이름표는 여기서 만든다 — 침대 번호만으로는 관제사가 누구 자리인지 모른다.
    QVector<RoiZone> withLabels = roiZones_[channel];
    for (auto& z : withLabels) {
        z.label = QStringLiteral("침대 %1 · %2")
                      .arg(z.id + 1)
                      .arg(zoneResidentName(channel, z.id));
    }

    if (channelViews[channel]) channelViews[channel]->setZones(withLabels);
    if (roiEditorView && roiEditChannel == channel) roiEditorView->setZones(withLabels);

    rebuildBedList();            // 인스펙터의 침대 목록(입소자 콤보 포함)
    refreshCamChannelStatus();   // 채널 타일 배지("침대 2")
}

void MainWindow::onRoiVisibilityToggled(bool on)
{
    for (auto* v : channelViews)
        if (v) v->setRoiVisible(on);
    if (roiEditorView) roiEditorView->setRoiVisible(on);
    if (roiToggleButton)
        roiToggleButton->setText(on ? QStringLiteral("영상에 표시")
                                    : QStringLiteral("표시 숨김"));
}

void MainWindow::onRoiCompleted(int channel, int roiId, const QPolygonF& normPts)
{
    if (channel < 0 || channel >= 4) return;

    sendRoi(channel, roiId, normPts);

    // 로컬 목록에도 반영 — 같은 id가 이미 있으면(다시 그린 것) 좌표만 갈아끼운다.
    bool replaced = false;
    for (auto& z : roiZones_[channel]) {
        if (z.id != roiId) continue;
        z.poly = normPts;
        replaced = true;
        break;
    }
    if (!replaced) {
        RoiZone z;
        z.id = roiId;
        z.poly = normPts;
        roiZones_[channel].push_back(z);
    }

    // 방금 그린 걸 볼 수 있도록 표시 토글이 꺼져 있으면 켠다
    if (roiToggleButton && !roiToggleButton->isChecked())
        roiToggleButton->setChecked(true);

    refreshRoiZones(channel);
    if (roiEditorView && roiEditChannel == channel)
        roiEditorView->setSelectedZone(roiId);   // 바로 입소자를 지정하도록 선택 유지

    qDebug() << "침대 ROI 전송: ch" << channel << "침대" << roiId
             << "," << normPts.size() << "점";
}

// 침대를 클릭해 편집 대상으로 고름 — 인스펙터 목록의 강조를 맞춘다.
void MainWindow::onRoiZoneSelected(int channel, int roiId)
{
    Q_UNUSED(channel);
    Q_UNUSED(roiId);
    rebuildBedList();
}

void MainWindow::sendRoi(int channel, int roiId, const QPolygonF& normPts, bool clear)
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
    // reserved 하위 8비트가 침대 번호다 (프로토콜 DBJ_CTRL_ROI_ID 참조).
    // 삭제 시 kRoiIdAll을 실으면 그 채널의 침대를 전부 지운다.
    h.reserved = static_cast<uint16_t>(roiId & 0xFF);

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

// "이 침대는 이 사람 자리" 를 서버에 통보한다.
// 서버는 이 매핑을 roi_zones에 남기고, 그 침대에서 생긴 추적 객체에 입소자를
// 귀속시켜 낙상·이탈 알림에 이름을 붙인다(server/core/identity_tracker.hpp).
void MainWindow::sendRoiBind(int channel, int roiId, int residentId)
{
    QTcpSocket* sock = socketForChannel(channel);
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
        return;
    }

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlRoiBind;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = 0;

    dbj_roi_bind_t b;
    b.roi_id = static_cast<uint8_t>(roiId & 0xFF);
    b.reserved = 0;
    b.resident_id = static_cast<uint32_t>(residentId > 0 ? residentId : 0);

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    pkt.append(reinterpret_cast<const char*>(&b), sizeof(b));
    sock->write(pkt);
    sock->flush();
    qDebug() << "침대 매핑 전송: ch" << channel << "침대" << roiId
             << "→ 입소자" << residentId;
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

// ═══════════════════════════════════════════════════════════
//  카메라 이미지 파라미터 전송 — 슬라이더 값(0~100)을 IMAGE_SET 제어 메시지로
//  담당 Pi 서버에 보낸다. 실제 카메라 적용(ONVIF/SUNAPI)은 서버가 수행한다.
//  (sendCamera와 동일한 패턴 — 헤더 뒤에 4바이트 dbj_image_params_t를 붙임)
// ═══════════════════════════════════════════════════════════
void MainWindow::sendImageParams(int channel, int b, int c, int s)
{
    QTcpSocket* sock = socketForChannel(channel);   // 이 채널 담당 Pi 소켓
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
        return;
    }

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlImageSet;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = sizeof(dbj_image_params_t);   // 이어지는 파라미터 바이트 수

    dbj_image_params_t ip;
    ip.brightness = static_cast<uint8_t>(qBound(0, b, 100));
    ip.contrast   = static_cast<uint8_t>(qBound(0, c, 100));
    ip.saturation = static_cast<uint8_t>(qBound(0, s, 100));

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h),  sizeof(h));
    pkt.append(reinterpret_cast<const char*>(&ip), sizeof(ip));
    sock->write(pkt);
    sock->flush();
    // 어떤 채널이 어느 Pi로 나가는지 확인용 — CH2/3/4가 안 먹으면 서버 IMAGE_SET 핸들러 점검.
    qDebug() << "➔ [Qt→서버] IMAGE_SET ch" << (channel + 1)
             << "(Pi" << serverForChannel(channel) << ") b=" << b << "c=" << c << "s=" << s;
}

// 카메라 초점 제어 — FOCUS_SET 제어 메시지로 담당 Pi에 전송. 실제 적용(SUNAPI
// SimpleFocus)은 서버가 수행. area=false 전체 자동초점, true 클릭 지점 영역 초점.
void MainWindow::sendFocus(int channel, bool area, float nx, float ny)
{
    QTcpSocket* sock = socketForChannel(channel);
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
        return;
    }

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlFocusSet;
    h.channel = static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = sizeof(dbj_focus_t);

    dbj_focus_t f;
    f.mode = area ? kFocusArea : kFocusWhole;
    f.reserved = 0;
    f.x = static_cast<uint16_t>(qBound(0, int(nx * kRoiCoordScale), kRoiCoordScale));
    f.y = static_cast<uint16_t>(qBound(0, int(ny * kRoiCoordScale), kRoiCoordScale));

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    pkt.append(reinterpret_cast<const char*>(&f), sizeof(f));
    sock->write(pkt);
    sock->flush();
}

// imgAfter(실시간 프리뷰) 클릭 → 클릭한 지점에 영역 초점. 레터박스(KeepAspectRatio)
// 여백을 빼고 실제 표시된 이미지 안에서의 정규화 좌표를 계산한다.
bool MainWindow::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == imgAfter && ev->type() == QEvent::MouseButtonPress) {
        if (imgAfter->hasFrame()) {
            const QRectF r = imgAfter->imageRect();   // 레터박스 안 실제 영상 사각형
            auto* me = static_cast<QMouseEvent*>(ev);
            const double px = me->position().x() - r.left();
            const double py = me->position().y() - r.top();
            if (px >= 0 && py >= 0 && px < r.width() && py < r.height()) {
                sendFocus(roiEditChannel, true,
                          float(px / r.width()), float(py / r.height()));
            }
        }
        return true;   // 이 클릭은 소비
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

    // 적용 / 초기화
    auto* apply = new QPushButton(QStringLiteral("적용"));
    apply->setObjectName("camPrimary");   // 이 페이지의 주 액션
    apply->setCursor(Qt::PointingHandCursor);
    auto* reset = new QPushButton(QStringLiteral("초기화"));
    reset->setObjectName("roiClear");
    reset->setCursor(Qt::PointingHandCursor);
    auto* br = new QHBoxLayout();
    br->addStretch();
    br->addWidget(reset);
    br->addWidget(apply);
    col->addLayout(br);

    // 포커스 구분선 캡션
    auto* fcap = new QLabel(QStringLiteral("초점"));
    fcap->setObjectName("camSectionCap");
    col->addWidget(fcap);
    auto* afBtn = new QPushButton(QStringLiteral("전체 자동초점"));
    afBtn->setObjectName("roiButton");
    afBtn->setCursor(Qt::PointingHandCursor);
    col->addWidget(afBtn);
    auto* focusHint = new QLabel(
        QStringLiteral("💡 오른쪽 실시간 영상을 클릭하면 그 지점에 초점을 맞춥니다."));
    focusHint->setObjectName("camHint");
    focusHint->setWordWrap(true);
    col->addWidget(focusHint);

    connect(afBtn, &QPushButton::clicked, this, [this]() {
        sendFocus(roiEditChannel, false, 0.0f, 0.0f);
    });
    connect(apply, &QPushButton::clicked, this, [this]() {
        const int ch = roiEditChannel;
        // 적용 직전 현재 프레임을 Before 스냅샷으로 고정
        if (imgBefore && !lastFramePix_[ch].isNull())
            imgBefore->setFrame(lastFramePix_[ch]);
        sendImageParams(ch, imgBright->value(), imgContrast->value(),
                        imgSaturation->value());
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        for (ClickSlider* s : {imgBright, imgContrast, imgSaturation})
            s->setValue(50);
    });

    for (ClickSlider* s : {imgBright, imgContrast, imgSaturation})
        s->setValue(50);   // 중앙값에서 시작
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
    camV->addWidget(cap);

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

    // 액션 버튼 줄: 검색 / 연결 / 해제
    auto* btnRow = new QHBoxLayout();
    searchCameraButton = new QPushButton(QStringLiteral("🔍 같은 망 카메라 검색"));
    searchCameraButton->setObjectName("roiButton");
    searchCameraButton->setCursor(Qt::PointingHandCursor);
    connect(searchCameraButton, &QPushButton::clicked, this, &MainWindow::onSearchCameraClicked);
    addCameraButton = new QPushButton(QStringLiteral("📷 연결"));
    addCameraButton->setObjectName("camPrimary");   // 이 페이지의 주 액션
    addCameraButton->setCursor(Qt::PointingHandCursor);
    connect(addCameraButton, &QPushButton::clicked, this, &MainWindow::onAddCameraClicked);
    clearCameraButton = new QPushButton(QStringLiteral("해제"));
    clearCameraButton->setObjectName("roiClear");
    clearCameraButton->setCursor(Qt::PointingHandCursor);
    connect(clearCameraButton, &QPushButton::clicked, this, &MainWindow::onCameraClearClicked);
    btnRow->addWidget(searchCameraButton);
    btnRow->addStretch();
    btnRow->addWidget(addCameraButton);
    btnRow->addWidget(clearCameraButton);
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
            if (cam.ip.isEmpty()) continue;

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
    auto zones = roiZones_[ch];
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

    // 1) 이미지 Before/After 프리뷰
    auto* imgPage = new QWidget();
    auto* iv = new QVBoxLayout(imgPage);
    iv->setContentsMargins(12, 10, 12, 12);   // 스테이지 카드 안쪽 여백
    iv->setSpacing(8);
    auto* capRow = new QHBoxLayout();
    capRow->setSpacing(10);
    for (const QString& t : {QStringLiteral("적용 전"), QStringLiteral("적용 후 (실시간)")}) {
        auto* c = new QLabel(t);
        c->setObjectName("camStageCap");
        capRow->addWidget(c, 1, Qt::AlignHCenter);
    }
    // FramePreview는 원본 프레임을 그대로 들고 paintEvent에서 한 번만 그린다
    // (QLabel::setPixmap처럼 매 프레임 레이아웃을 무효화하지 않는다 — 지연의 원인이었다).
    imgBefore = new FramePreview(QStringLiteral("적용 전"));
    imgAfter  = new FramePreview(QStringLiteral("실시간"));
    for (FramePreview* p : {imgBefore, imgAfter}) {
        p->setMinimumSize(320, 200);
        p->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    auto* pv = new QHBoxLayout();
    pv->setSpacing(10);
    pv->addWidget(imgBefore, 1);
    pv->addWidget(imgAfter, 1);
    iv->addLayout(capRow);
    iv->addLayout(pv, 1);
    camStageStack->addWidget(imgPage);

    // 실시간 프리뷰 클릭 → 클릭 지점 영역 초점 (eventFilter가 처리)
    imgAfter->installEventFilter(this);
    imgAfter->setCursor(Qt::PointingHandCursor);

    // 실시간(After) 갱신은 폴링하지 않는다 — onReadyRead가 프레임이 도착할 때마다
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
    if (imgBefore) imgBefore->clearFrame();
    if (imgAfter)  imgAfter->setFrame(lastFramePix_[ch]);  // 빈 프레임이면 안내 문구

    refreshCamChannelStatus();
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
    // (이후는 onReadyRead가 프레임마다 갱신).
    if (imageMode && imgAfter) imgAfter->setFrame(lastFramePix_[roiEditChannel]);
}

// 채널별 연결·ROI 지정 여부를 레일 배지에 반영.
void MainWindow::refreshCamChannelStatus()
{
    for (int ch = 0; ch < 4; ++ch) {
        if (!camChannelStatus[ch]) continue;
        const bool connected = cameraActive_[ch];
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
        const bool on = cameraActive_[cur];
        // 채널 상태 텍스트와 같은 어휘(●/○)를 붙여 두 배지의 표기를 통일한다.
        camInspPill->setText(on ? QStringLiteral("● 연결됨") : QStringLiteral("○ 미연결"));
        camInspPill->setProperty("connected", on);
        camInspPill->style()->unpolish(camInspPill);
        camInspPill->style()->polish(camInspPill);
        camInspPill->update();
    }
    if (camInspIp) {
        // URL에는 계정·비밀번호가 들어 있으므로 호스트만 보여준다.
        const QString host = QUrl(lastCameraUrl_[cur]).host();
        camInspIp->setText(host.isEmpty()
                               ? QStringLiteral("카메라가 연결되지 않았습니다")
                               : QStringLiteral("%1 · RTSP profile2").arg(host));
    }
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
    // 다음 프레임부터 onReadyRead가 이 편집기에 실시간 영상을 계속 넣어준다.
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

    // IP 하나 → 4채널 URL 생성 → 채널별 담당 Pi로 전송(socketForChannel이 라우팅).
    int sent = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const QString url = buildRtspUrl(ip, user, password, port, prof, ch);
        lastCameraUrl_[ch] = url;   // 재접속 시 자동 재전송용(세션 한정)
        cameraActive_[ch] = true;   // 연결됨(재시작 후에도 해제 가능하도록 지속 저장)
        videoSuppressed_[ch] = false;  // 프레임 표시 재개
        if (channelViews[ch])
            channelViews[ch]->setCameraConnected(true);  // "신호 대기 중…" 표시
        if (sendCamera(ch, url)) ++sent;
    }
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
    discoveryTable->setRowCount(0);
    syncDiscoveryTableHeight();   // 결과 지웠으니 다시 숨김
    discoverySeen.clear();
    if (discoveryStatus)
        discoveryStatus->setText(QStringLiteral("같은 망의 ONVIF 카메라를 검색 중…"));

    const QByteArray probe = buildWsDiscoveryProbe();
    const QHostAddress mcast(QStringLiteral("239.255.255.250"));
    QUdpSocket* sock = discoverySocket;

    // 기본 멀티캐스트 인터페이스가 가상 어댑터로 잡히면 카메라가 Probe를 못 받는다
    // → IPv4·멀티캐스트 가능한 모든 인터페이스로 각각 쏜다.
    auto sendProbes = [sock, probe, mcast]() {
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
            sock->setMulticastInterface(iface);
            if (sock->writeDatagram(probe, mcast, 3702) > 0) ++sentOn;
        }
        if (sentOn == 0) sock->writeDatagram(probe, mcast, 3702);  // 폴백
    };

    sendProbes();                                  // UDP 유실·타이밍 대비 여러 번 재전송
    QTimer::singleShot(700, this, sendProbes);
    QTimer::singleShot(1600, this, sendProbes);
    QTimer::singleShot(3000, this, sendProbes);
    QTimer::singleShot(6500, this, [this]() {
        if (discoveryStatus && discoveryTable)
            discoveryStatus->setText(
                QStringLiteral("검색 완료 — %1대 발견 (행을 클릭하면 IP가 채워집니다)")
                    .arg(discoveryTable->rowCount()));
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
        videoSuppressed_[ch] = true;    // 이후 들어오는 잔여 프레임 무시(검은 화면 유지)
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
    if (imgAfter) imgAfter->clearFrame();
    if (imgBefore) imgBefore->clearFrame();
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
        if (cameraActive_[ch]) {
            videoSuppressed_[ch] = false;
            if (channelViews[ch]) channelViews[ch]->setCameraConnected(true);
        }
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
            videoSuppressed_[ch] = false;  // 프레임 표시 재개
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
        if (fallActive[channel] || bedEgressActive[channel] || vitalAbnormalActive[channel]) {
            // 1. 빨간 테두리 끄고 로컬 경보 상태 클리어 (낙상·침상이탈·생체이상 모두)
            fallActive[channel] = false;
            bedEgressActive[channel] = false;
            vitalAbnormalActive[channel] = false;
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

            // 서버로 보내는 0x03 은 "모자이크를 다시 씌워라"는 뜻이고, 현장의
            // 사이렌·LED 는 알림 노드가 들고 있어 브로커를 통해 따로 꺼야 한다.
            // 경로가 달라 TCP 성공 여부와 무관하게 보낸다.
            if (mqtt) mqtt->sendAlarmClear(patients[channel].room);
        }
    }

    // (flush는 위 루프에서 채널별 소켓마다 이미 처리)
    refreshAlarmButton();   // 모두 해제됐으니 버튼을 차분한 아웃라인으로 되돌린다
    setVideoFocus(-1);      // 경보 해제 → 균등 2×2로 복귀
    if (!packetSent) {
        // 현재 활성화된 경보가 아예 없을 때만 안내 메시지 표시
        QMessageBox::information(this, QStringLiteral("경보 해제"),
                                 QStringLiteral("현재 활성화된 낙상/침상이탈 경보가 없습니다."));
    }
}

// 활성 경보 목록을 만든다 — 등급 판정(critical/high/medium)과 도형 선택이 전부
// 여기서 끝나 AlertBanner로는 완성된 값만 넘어간다(D-02). 채널당 최대
// 3건(낙상/침상이탈/생체이상)이 나올 수 있다.
QList<AlertItem> MainWindow::collectAlertItems() const
{
    QList<AlertItem> items;
    for (int ch = 0; ch < 4; ++ch) {
        if (fallActive[ch]) {
            const QString severity = QStringLiteral("critical");
            items.append({ch, severityGlyph(severity), QStringLiteral("낙상"),
                          patients[ch].bed, severity});
        }
        if (bedEgressActive[ch]) {
            const QString severity = QStringLiteral("high");
            items.append({ch, severityGlyph(severity), QStringLiteral("침상이탈"),
                          patients[ch].bed, severity});
        }
        if (vitalAbnormalActive[ch]) {
            // 그 채널에 배정된 입소자의 현재 바이탈 등급을 쓴다 — 단일 출처는
            // vitalSeverity()(D-02). 배정된 사람을 찾지 못하면 안전하게
            // medium으로 내린다(UI-SPEC §4.2).
            QString severity = QStringLiteral("medium");
            const QVector<int>& ids = residentsByChannel_[ch];
            if (!ids.isEmpty()) {
                const VitalSample sample = vitals_.value(ids.first());
                severity = vitalSeverity(sample.spo2, sample.heartRate);
            }
            items.append({ch, severityGlyph(severity), QStringLiteral("생체신호 이상"),
                          patients[ch].bed, severity});
        }
    }
    return items;
}

// 낙상/침상이탈이 하나라도 활성이면 경보 버튼을 빨강 채움으로, 아니면 차분한 아웃라인으로.
void MainWindow::refreshAlarmButton()
{
    if (!alarmClearButton) return;
    bool anyActive = false;
    for (int ch = 0; ch < 4; ++ch)
        if (fallActive[ch] || bedEgressActive[ch] || vitalAbnormalActive[ch]) { anyActive = true; break; }
    updateAlarmBanner();   // 경보 배너 표시/문구 갱신 (활성 시에만 노출)
    // 상시 노출용 배너(#alertBanner) 갱신 — 상태를 바꾸는 다섯 지점이 전부
    // 이 함수를 거치므로 배선은 여기 한 곳뿐이다.
    if (alertBanner_) alertBanner_->setActiveAlerts(collectAlertItems());

    // 경보가 하나라도 활성이면 창 전체 테두리 빨강 펄스, 아니면 끈다.
    if (alarmOverlay_) {
        auto* ov = static_cast<AlarmOverlay*>(alarmOverlay_);
        if (anyActive) { ov->setGeometry(ui->centralwidget->rect()); ov->start(); }
        else           ov->stop();
    }
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

    // 영상을 열었으므로 이 이벤트는 '확인' 처리
    markLogConfirmed(row);

    // 이벤트↔NVR 연결용 — 이 행의 채널/시각을 기억해뒀다가 "NVR에서 이어보기"에서 쓴다.
    const QVariant chVar = item->data(Qt::UserRole + 1);
    const QVariant tsVar = item->data(Qt::UserRole + 2);
    selectedEventChannel_ = chVar.isValid() ? chVar.toInt() : -1;
    selectedEventTimestampMs_ = tsVar.isValid() ? tsVar.toLongLong() : -1;
    if (nvrJumpButton)
        nvrJumpButton->setEnabled(selectedEventChannel_ >= 0 && selectedEventTimestampMs_ >= 0);

    qDebug() << "블랙박스 재생 요청 —" << url;
    // 인라인 플레이어(페이지 우측)에서 바로 재생 — 팝업 없음.
    playBlackboxClip(url);
}

// 상태 컬럼(3번)을 '미확인' → '확인'으로 바꾸고 초록색으로 표시.
void MainWindow::markLogConfirmed(int row)
{
    if (!logTable || row < 0 || row >= logTable->rowCount()) return;

    auto* statusItem = logTable->item(row, 3);
    if (!statusItem) return;
    if (statusItem->text() == QStringLiteral("확인")) return;   // 이미 확인됨

    // 텍스트 변경 중 자동 재정렬로 행이 움직여 엉뚱한 셀을 건드리는 것 방지
    const bool wasSorting = logTable->isSortingEnabled();
    logTable->setSortingEnabled(false);

    statusItem->setText(QStringLiteral("확인"));

    logTable->setSortingEnabled(wasSorting);
    refreshEventLog();   // 상태 배지 색 + 요약(미확인 수) 갱신
}

// 로그가 바뀔 때마다 호출 — 이벤트/상태 셀을 색으로 구분한다.
//  · 이벤트: 낙상=빨강, 침상이탈=주황   · 상태: 미확인=빨강, 확인=초록
void MainWindow::refreshEventLog()
{
    if (!logTable) return;

    const QColor cCritical(QString::fromLatin1(kCritical));
    const QColor cWarn(QString::fromLatin1(kWarn));
    const QColor cNormal(QString::fromLatin1(kNormal));
    const QColor cSub(QString::fromLatin1(kTextSub));

    for (int r = 0; r < logTable->rowCount(); ++r) {
        auto* evtItem = logTable->item(r, 2);
        auto* stItem = logTable->item(r, 3);
        if (!evtItem || !stItem) continue;

        const QString evt = QString(evtItem->text()).remove(QLatin1Char(' '));
        const bool isFall = (evt == QStringLiteral("낙상"));

        // 이벤트 셀 색 + 가운데 정렬
        evtItem->setForeground(isFall ? cCritical : cWarn);
        evtItem->setTextAlignment(Qt::AlignCenter);
        if (auto* chItem = logTable->item(r, 1)) chItem->setTextAlignment(Qt::AlignCenter);

        // 상태 셀 색 + 가운데 정렬
        const bool confirmed = (stItem->text() == QStringLiteral("확인"));
        stItem->setForeground(confirmed ? cNormal : cCritical);
        stItem->setTextAlignment(Qt::AlignCenter);
        if (auto* dt = logTable->item(r, 0)) dt->setForeground(cSub);
    }
}

// 이벤트 드롭다운(+검색 버튼일 땐 날짜 범위까지) 조건에 맞는 행만 표시.
// 행을 지우지 않고 숨김 처리라 조건을 바꾸면 즉시 원복된다.
void MainWindow::applyLogFilters(bool withDates)
{
    if (!logTable) return;

    const QString evtSel = filterEventType ? filterEventType->currentText() : QString();
    const bool evtAll = evtSel.isEmpty() || evtSel == QStringLiteral("전체 이벤트");
    // 콤보는 "침상이탈", 로그 행은 "침상 이탈" — 띄어쓰기 차이를 흡수해 비교
    const QString evtKey = QString(evtSel).remove(QLatin1Char(' '));

    for (int row = 0; row < logTable->rowCount(); ++row) {
        bool show = true;

        // 이벤트 종류
        if (!evtAll) {
            auto* evtItem = logTable->item(row, 2);
            const QString evt =
                evtItem ? QString(evtItem->text()).remove(QLatin1Char(' ')) : QString();
            show = (evt == evtKey);
        }

        // 날짜 범위 — 0열 "yyyy-MM-dd HH:mm:ss"의 앞 10글자만 파싱
        if (show && withDates && filterDateFrom && filterDateTo) {
            auto* dtItem = logTable->item(row, 0);
            const QDate d = dtItem
                                ? QDate::fromString(dtItem->text().left(10), QStringLiteral("yyyy-MM-dd"))
                                : QDate();
            show = d.isValid() && d >= filterDateFrom->date() && d <= filterDateTo->date();
        }

        logTable->setRowHidden(row, !show);
    }

    refreshEventLog();   // 필터/삽입 후 행 색·요약 카드 갱신
}


// residents(재원)를 읽어 두 가지를 채운다.
//  1) patients[] — 채널당 대표 1명. 영상 오버레이·케어 타임 카드처럼 "장소" 단위로
//     보여주는 화면이 쓴다(그 자리에 카드가 하나뿐이라 여러 명을 담을 수 없다).
//  2) residentInfo_ / residentsByChannel_ / wearableToResident — 사람 단위. 바이탈은
//     기기가 사람마다 달라 한 명씩 따로 보여줘야 하므로 이쪽을 쓴다.
// 채널에 등록된 입소자가 없으면 "미배정"으로 둔다(하드코딩 이름 없음).
void MainWindow::loadPatientsFromDb()
{
    for (int ch = 0; ch < 4; ++ch) {
        patients[ch] = { QStringLiteral("미배정"),
                         QStringLiteral("채널 %1").arg(ch + 1),
                         QString() };
        residentsByChannel_[ch].clear();
    }
    wearableToResident.clear();
    residentInfo_.clear();

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT resident_id, camera_id, name, room, wearable_id FROM residents "
            "WHERE status='재원' AND camera_id BETWEEN 0 AND 3 "
            "ORDER BY camera_id, resident_id"))) {
        qDebug() << "채널 환자 매핑 조회 실패:" << q.lastError().text();
        return;
    }

    bool assigned[4] = {};
    while (q.next()) {
        const int rid = q.value(0).toInt();
        const int ch  = q.value(1).toInt();
        if (ch < 0 || ch >= 4) continue;

        ResidentInfo info;
        info.name    = q.value(2).toString();
        info.room    = q.value(3).toString();
        // 위치는 채널로만 표기. 침상 구분이 생기기 전까지는 같은 채널이면 같은 값.
        info.bed     = QStringLiteral("채널 %1").arg(ch + 1);
        info.channel = ch;
        residentInfo_.insert(rid, info);
        residentsByChannel_[ch].append(rid);

        // 웨어러블은 그 채널의 모든 입소자 것을 등록한다. 브로커는 기기 id로만
        // 알려주므로, 여기 등록이 빠지면 그 기기가 보낸 값은 주인을 몰라 버려진다.
        const QString wearable = q.value(4).toString().trimmed();
        if (!wearable.isEmpty()) wearableToResident.insert(wearable, rid);

        if (assigned[ch]) continue;   // 오버레이·케어 카드는 채널당 대표 1명만
        assigned[ch] = true;
        patients[ch].name = info.name;
        patients[ch].bed  = info.bed;
        patients[ch].room = info.room;
    }

    // 목록에서 빠진 입소자(퇴원 등)의 값·이력은 버린다. 남겨두면 나중에 재입원했을 때
    // 몇 달 전 그래프가 되살아나고, 계속 쌓이기만 해서 줄어들 일이 없다.
    for (auto it = vitals_.begin(); it != vitals_.end(); ) {
        if (residentInfo_.contains(it.key())) ++it;
        else it = vitals_.erase(it);
    }
    for (auto it = hrHistory_.begin(); it != hrHistory_.end(); ) {
        if (residentInfo_.contains(it.key())) ++it;
        else it = hrHistory_.erase(it);
    }
}

// 입소자 정보를 실제 화면에 다시 반영한다.
// 영상 오버레이는 채널당 한 줄이라 대표 1명(patients[])을 쓰고,
// 바이탈 카드는 입소자 수만큼 개수가 달라져 통째로 다시 만든다.
// 케어 타임 대시보드는 updateCareTime()에서 patients[]를 직접 읽으므로 여기선 제외.
void MainWindow::refreshPatientLabels()
{
    for (int ch = 0; ch < 4; ++ch) {
        // 침대 이름표·매핑 콤보도 새 입소자 구성으로 다시 그린다 — 이름을 고치거나
        // 퇴원시켰는데 침대 라벨만 옛 이름으로 남으면 관제사가 오판한다.
        refreshRoiZones(ch);
    }
    rebuildVitalCards();
}

void MainWindow::onResidentSearch()
{
    // 검색창 텍스트로 필터. 비어 있으면 재원자 카드만 다시 그린다.
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text()
                                            : QString());
}

// 선택한 입소자를 편집 폼 필드에 채운다(다이얼로그 열기 전 호출).
void MainWindow::loadResidentIntoForm(int id)
{
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
    // value(1)=room, value(2)=bed 는 병실/침대 제거로 폼에 반영하지 않는다.
    // DB는 0~3으로 저장, 화면엔 1~4로 보여준다(사람이 읽기 쉬운 채널 번호).
    editCameraId->setText(q.value(3).isNull() ? QString()
                                              : QString::number(q.value(3).toInt() + 1));
    editWearableId->setText(q.value(4).isNull() ? QString() : q.value(4).toString());
    setCombo(editRiskLevel, q.value(5).toString());
    if (q.value(6).toDate().isValid())  editAdmittedAt->setDate(q.value(6).toDate());
    if (q.value(7).toDate().isValid())  editDischargeDue->setDate(q.value(7).toDate());
    setCombo(editStatus, q.value(8).toString());
    editGuardianName->setText(q.value(9).toString());
    editGuardianPhone->setText(q.value(10).toString());
    editGuardianRelation->setText(q.value(11).toString());
    editNotes->setPlainText(q.value(12).toString());

    refreshAdmissionTable(selectedResidentId);

    qDebug() << "입소자 선택 — ID:" << selectedResidentId;
}

void MainWindow::onNewResident()
{
    selectedResidentId = -1;
    editName->clear();
    editCameraId->clear();
    editWearableId->clear();
    editGuardianName->clear();
    editGuardianPhone->clear();
    editGuardianRelation->clear();
    editNotes->clear();
    editRiskLevel->setCurrentIndex(1);   // 위험도 기본 '중'
    editStatus->setCurrentText(QStringLiteral("재원"));  // 신규는 항상 재원으로 시작
    editAdmittedAt->setDate(QDate::currentDate());
    editDischargeDue->setDate(QDate::currentDate().addMonths(1));
    refreshAdmissionTable(-1);   // 아직 저장 안 된 신규 → 이력 없음
    editName->setFocus();
}

// 저장 직전 DB의 값 — 라벨→문자열 맵. (변경 "전" 스냅샷)
QMap<QString, QString> MainWindow::snapshotResident(int id)
{
    QMap<QString, QString> m;
    QStringList cols;
    for (const auto& f : kLoggedFields) cols << QLatin1String(f.column);

    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT %1 FROM residents WHERE resident_id=?")
                  .arg(cols.join(QLatin1Char(','))));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return m;

    int i = 0;
    for (const auto& f : kLoggedFields) {
        const QVariant v = q.value(i++);
        QString text = v.isNull() ? QString() : v.toString();
        // camera_id는 DB(0~3) → 표시(1~4)로 맞춰 폼 스냅샷과 같은 기준으로 비교/기록.
        if (!v.isNull() && QLatin1String(f.column) == QLatin1String("camera_id"))
            text = QString::number(v.toInt() + 1);
        m.insert(QString::fromUtf8(f.label), text);
    }
    return m;
}

// 현재 폼에 입력된 값 — 위와 같은 라벨 체계로 (변경 "후" 스냅샷)
QMap<QString, QString> MainWindow::formSnapshot() const
{
    QMap<QString, QString> m;
    m[QStringLiteral("이름")]        = editName->text().trimmed();
    m[QStringLiteral("카메라 채널")] = editCameraId->text().trimmed();
    m[QStringLiteral("웨어러블 ID")] = editWearableId->text().trimmed();
    m[QStringLiteral("위험도")]      = editRiskLevel->currentText();
    m[QStringLiteral("입원일")]      = editAdmittedAt->date().toString(Qt::ISODate);
    m[QStringLiteral("퇴원 예정일")] = editDischargeDue->date().toString(Qt::ISODate);
    m[QStringLiteral("상태")]        = editStatus->currentText();
    m[QStringLiteral("보호자 이름")] = editGuardianName->text().trimmed();
    m[QStringLiteral("보호자 전화")] = editGuardianPhone->text().trimmed();
    m[QStringLiteral("보호자 관계")] = editGuardianRelation->text().trimmed();
    m[QStringLiteral("특이사항")]    = editNotes->toPlainText();
    return m;
}

// 그 입소자의 "가장 최근" 입원 에피소드 id (없으면 -1)
int MainWindow::currentAdmissionId(int residentId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT admission_id FROM admissions WHERE resident_id=? "
        "ORDER BY admitted_at DESC, admission_id DESC LIMIT 1"));
    q.addBindValue(residentId);
    return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
}

// before↔after를 비교해 달라진 필드만 resident_changes에 남긴다.
void MainWindow::logChanges(int residentId, int admissionId,
                            const QMap<QString, QString>& before,
                            const QMap<QString, QString>& after,
                            const QString& changeType)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "INSERT INTO resident_changes "
        "(resident_id, admission_id, field_label, old_value, new_value, "
        " change_type, changed_by) VALUES (?,?,?,?,?,?,?)"));

    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const QString oldV = before.value(it.key());
        if (oldV == it.value()) continue;      // 안 바뀐 건 로그 남기지 않음
        q.bindValue(0, residentId);
        q.bindValue(1, admissionId > 0 ? QVariant(admissionId) : QVariant());
        q.bindValue(2, it.key());
        q.bindValue(3, oldV);
        q.bindValue(4, it.value());
        q.bindValue(5, changeType);
        q.bindValue(6, currentUser.name);      // 로그인 사용자 = 작업자
        if (!q.exec())
            qDebug() << "변경 로그 기록 실패:" << q.lastError().text();
    }
}

void MainWindow::onSaveResident()
{
    if (editName->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("이름을 입력해주세요."));
        return;
    }

    // 카메라 채널은 화면에서 1~4로 입력받되 DB엔 0~3으로 저장한다.
    const QString camText = editCameraId->text().trimmed();
    if (!camText.isEmpty()) {
        bool ok = false;
        const int camNo = camText.toInt(&ok);
        if (!ok || camNo < 1 || camNo > 4) {
            QMessageBox::warning(this, QStringLiteral("입력 오류"),
                                 QStringLiteral("카메라 채널은 1~4 사이로 입력해주세요."));
            return;
        }
    }

    // 빈 칸은 NULL로 저장 (wearable_id는 문자열)
    // 화면 채널(1~4) → DB 채널(0~3). 빈 칸은 NULL.
    auto channelOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t.toInt() - 1);
    };
    auto textOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t);
    };

    const bool isNew = (selectedResidentId < 0);

    // 변경 전 값은 UPDATE 실행 전에 읽어둬야 한다 (실행 후엔 이미 덮어써짐).
    const QMap<QString, QString> before =
        isNew ? QMap<QString, QString>() : snapshotResident(selectedResidentId);

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
    // 병실/침대는 UI에서 제거했지만 컬럼이 NOT NULL이라 빈 문자열로 채운다(위치는 채널로 표기).
    // 주의: QString()은 SQL NULL로 들어가 NOT NULL 위반 → 반드시 non-null 빈 문자열.
    q.addBindValue(QStringLiteral(""));
    q.addBindValue(QStringLiteral(""));
    q.addBindValue(channelOrNull(editCameraId->text()));
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

    if (isNew) {
        selectedResidentId = q.lastInsertId().toInt();
        // 신규 등록이면 첫 입원 에피소드를 만든다.
        QSqlQuery a;
        a.prepare(QStringLiteral(
            "INSERT INTO admissions "
            "(resident_id, admitted_at, discharge_due, status, room, bed) "
            "VALUES (?,?,?,'재원','','')"));
        a.addBindValue(selectedResidentId);
        a.addBindValue(editAdmittedAt->date());
        a.addBindValue(editDischargeDue->date());
        if (!a.exec()) qDebug() << "입원 에피소드 생성 실패:" << a.lastError().text();
    }

    const int admId = currentAdmissionId(selectedResidentId);
    logChanges(selectedResidentId, admId, before, formSnapshot(),
               isNew ? QStringLiteral("등록") : QStringLiteral("수정"));




    // 화면 채널(1~4) → 내부 채널(0~3). 빈 칸이면 -1이 되어 전송 조건에서 걸러진다.
    // camText는 함수 앞부분(입력 검증)에서 이미 editCameraId 값으로 잡아둠.
    int cameraId = camText.isEmpty() ? -1 : camText.toInt() - 1;

    // 4채널 중 올바른 채널이고, 서버 소켓이 정상 연결된 상태일 때만 전송
    QTcpSocket* riskSock =
        (cameraId >= 0 && cameraId < 4) ? socketForChannel(cameraId) : nullptr;
    if (cameraId >= 0 && cameraId < 4 &&
        riskSock && riskSock->state() == QAbstractSocket::ConnectedState)
    {
        // 1. 위험도 텍스트를 서버가 이해하는 숫자 코드로 매칭 (하:1, 중:2, 상:3)
        int32_t statusVal = 1; 
        QString risk = editRiskLevel->currentText();
        if (risk == QStringLiteral("상")) statusVal = 3;
        else if (risk == QStringLiteral("중")) statusVal = 2;
        else if (risk == QStringLiteral("하")) statusVal = 1;

        // 2. 대상 침대 결정.
        //    위험도는 사람에게 붙는 값이라 채널이 아니라 침대 단위로 보낸다 —
        //    한 채널에 '상'과 '하'가 같이 누워 있으면 채널 일괄 적용은 둘 중
        //    하나를 반드시 틀리게 만든다. 이 입소자에게 배정된 침대들만 골라
        //    보내고, 아직 침대가 매핑돼 있지 않으면 예전처럼 채널 일괄로 보낸다.
        QVector<int> targetBeds;
        for (auto it = roiResident_[cameraId].constBegin();
             it != roiResident_[cameraId].constEnd(); ++it) {
            if (it.value() == selectedResidentId && selectedResidentId > 0)
                targetBeds.push_back(it.key());
        }
        if (targetBeds.isEmpty()) targetBeds.push_back(kRoiIdAll);

        for (int bed : targetBeds) {
            // 3. 프로토콜 공용 제어 헤더 조립
            dbj_ctrl_header_t h;
            h.magic = kCtrlMagic;                   // 0xDB4C
            h.version = 0x01;
            h.type = 0x04;
            h.channel = static_cast<uint8_t>(cameraId);
            h.point_count = statusVal;
            h.reserved = static_cast<uint16_t>(bed & 0xFF);  // 하위 8비트 = 침대 번호

            // 4. 바이트 버퍼 생성 및 데이터 직렬화 후 소켓 방출
            QByteArray pkt;
            pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
            riskSock->write(pkt);
            riskSock->flush();
            qDebug() << "➔ [Qt -> 서버] 채널" << (cameraId + 1) << "침대"
                     << (bed == kRoiIdAll ? -1 : bed)
                     << "환자 위험도 변경 패킷 전송 완료 (값:" << statusVal << ")";
        }
    }

    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshAdmissionTable(selectedResidentId);
    refreshResidentDialogHeader();   // 편집 다이얼로그 상단 프로필도 갱신
    // 채널↔환자 매핑을 DB 기준으로 다시 읽어 영상/바이탈 이름을 갱신한다.
    // (채널 재배정·이름 변경·camera_id 해제 등 모든 경우를 한 번에 반영)
    loadPatientsFromDb();
    refreshPatientLabels();
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

    // 에피소드도 닫는다 — 이 날짜가 이력 테이블의 "퇴원일"로 표시된다.
    const int admId = currentAdmissionId(selectedResidentId);
    QSqlQuery a;
    a.prepare(QStringLiteral("UPDATE admissions SET discharged_at=CURDATE(), "
                             "status='퇴원' WHERE admission_id=?"));
    a.addBindValue(admId);
    a.exec();

    logChanges(selectedResidentId, admId,
               {{QStringLiteral("상태"), QStringLiteral("재원")}},
               {{QStringLiteral("상태"), QStringLiteral("퇴원")}},
               QStringLiteral("퇴원"));
    refreshAdmissionTable(selectedResidentId);

    // 폼의 상태 콤보도 '퇴원'으로 반영
    const int i = editStatus->findText(QStringLiteral("퇴원"));
    if (i >= 0) editStatus->setCurrentIndex(i);

    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshResidentDialogHeader();
    loadPatientsFromDb();    // 퇴원 → 그 채널을 "미배정"으로 되돌린다
    refreshPatientLabels();
    QMessageBox::information(this, QStringLiteral("퇴원 처리"),
                             QStringLiteral("퇴원 처리되었습니다."));
    qDebug() << "퇴원 처리 완료 — ID:" << selectedResidentId;
}

void MainWindow::onReadmitResident()
{
    if (selectedResidentId < 0) {
        QMessageBox::warning(this, QStringLiteral("선택 없음"),
                             QStringLiteral("재입원 처리할 입소자를 먼저 선택해주세요."));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("재입원 처리"),
            QStringLiteral("새 입원 기록을 만들까요?\n(이전 입원 이력은 그대로 보존됩니다)"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QSqlQuery a;
    a.prepare(QStringLiteral(
        "INSERT INTO admissions (resident_id, admitted_at, status, room, bed) "
        "VALUES (?, CURDATE(), '재원', '', '')"));
    a.addBindValue(selectedResidentId);
    if (!a.exec()) {
        QMessageBox::critical(this, QStringLiteral("재입원 실패"), a.lastError().text());
        return;
    }

    QSqlQuery r;
    r.prepare(QStringLiteral("UPDATE residents SET status='재원', admitted_at=CURDATE() "
                             "WHERE resident_id=?"));
    r.addBindValue(selectedResidentId);
    r.exec();

    logChanges(selectedResidentId, currentAdmissionId(selectedResidentId),
               {{QStringLiteral("상태"), QStringLiteral("퇴원")}},
               {{QStringLiteral("상태"), QStringLiteral("재원")}},
               QStringLiteral("재입원"));

    const int i = editStatus->findText(QStringLiteral("재원"));
    if (i >= 0) editStatus->setCurrentIndex(i);
    editAdmittedAt->setDate(QDate::currentDate());
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshAdmissionTable(selectedResidentId);
    refreshResidentDialogHeader();
    loadPatientsFromDb();    // 재입원 → 그 채널에 이름 복원
    refreshPatientLabels();
}
