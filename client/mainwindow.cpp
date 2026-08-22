#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "theme.h"
#include "thememanager.h"
#include "videoview.h"
#include "wintheme.h"
#include "sparkline.h"
#include "mqttqtmanager.h"
#include "alertmatrixpreview.h"
#include "timelinebar.h"
#include <QHostAddress>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslError>
#include <QApplication>       // 로그아웃 시 최상위 창 정리 + 이벤트 루프 종료
#include <QCoreApplication>   // MQTT/영상 스트림 CA 인증서를 실행 파일 기준 경로에서 찾는다
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
#include <QMenu>
#include <QMessageBox>
#include <QFileDialog>

#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QDateEdit>
#include <QCalendarWidget>
#include <QTextCharFormat>
#include <QSlider>
#include <QJsonObject>
#include <QPageLayout>
#include <QRect>
#include <QPdfWriter>
#include <QTextDocument>
#include <QBuffer>
#include <QPageSize>
#include <QDir>
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
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QNetworkInterface>
#include <QProcess>
#include <QLayout>
#include <algorithm>

// 디자인 토큰(kLight/kDark/kAccent…)은 theme.h로 분리했다 — 로그인 화면과 공유.
namespace {

// 한글 요일 한 글자. QDate::toString("ddd") 는 시스템 로케일을 따라가서 윈도우
// 영문 환경에선 "Thu" 가 나온다 — 화면과 PDF 가 같은 문서라 표기를 고정한다.
// (리포트 화면·PDF 두 곳에서 쓰므로 여기 파일 위쪽에 둔다)
QString koreanDow(const QDate& d)
{
    static const char* kDow[] = {"월", "화", "수", "목", "금", "토", "일"};
    const int i = d.dayOfWeek() - 1;   // Qt: 1=월 … 7=일
    return (i >= 0 && i < 7) ? QString::fromUtf8(kDow[i]) : QString();
}

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
    {"이름", "name"},               {"호실", "room"},
    {"카메라 채널", "camera_id"},
    {"웨어러블 ID", "wearable_id"}, {"위험도", "risk_level"},
    {"입원일", "admitted_at"},      {"퇴원 예정일", "discharge_due"},
    {"상태", "status"},             {"특이사항", "notes"},
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
const char* kDefaultHostA  = "172.20.32.79";
const char* kDefaultHostB  = "172.20.32.80";

// 서버 인덱스(0=Pi A, 1=Pi B) → 저장된 호스트(없으면 기본값).
QString serverHost(int idx) {
    QSettings s;
    return idx == 0 ? s.value(kSettingsHostA, kDefaultHostA).toString()
                    : s.value(kSettingsHostB, kDefaultHostB).toString();
}
// 채널(0~3) → 담당 Pi의 호스트 (블랙박스 클립 URL 등 host가 필요한 곳용).
// 매핑은 MainWindow::serverForChannel과 동일하게 유지할 것 (ch0,1→0 / ch2172.20.32.39,3→1).
QString hostForChannel(int ch) { return serverHost(ch < 2 ? 0 : 1); }

// 이 카메라(4채널) 한 대가 담당하는 방 이름. 지금은 room이 항상 하나뿐이라
// 값 하나로 충분하지만, 카메라를 더 붙여 room이 늘어나면 이 자리가 목록(예:
// QStringList)으로 바뀌는 지점이다 — serverHost()/hostForChannel()과 같은
// 패턴으로 QSettings에서 읽어와 하드코딩을 피한다.
const char* kSettingsRoomName = "room/name";
QString currentRoomName() {
    QSettings s;
    return s.value(kSettingsRoomName, QStringLiteral("101호")).toString();
}

// 리소스 트리에 세울 방 목록. 0번은 지금 카메라(4채널)가 실제로 담당하는 방이고,
// 1번부터는 아직 카메라가 붙지 않은 "자리"다 — 트리에 빈 채널 4개로만 보인다.
//
// 카메라를 진짜로 늘리려면 고정크기-4 배열 19곳과 setVideoFocus()의 그리드 배치를
// 먼저 정리해야 한다(mainwindow.h의 확장 지점 메모). 그 작업 전에도 방이 늘어나면
// 화면이 어떻게 갈라지는지 구조로 드러내려고 자리만 먼저 만들어 둔다.
const char* kSettingsRoomNames = "room/names";
QStringList roomNames() {
    QSettings s;
    const QStringList saved = s.value(kSettingsRoomNames).toStringList();
    if (!saved.isEmpty()) return saved;
    return { currentRoomName(), QStringLiteral("102호") };
}

// MQTT 브로커 주소. 영상 서버와 같은 라즈베리에 띄우는 경우가 많아 기본값을
// Pi A 와 같게 뒀지만, 브로커만 따로 두는 구성도 있어 설정으로 분리했다.
const char* kSettingsBrokerHost = "mqtt/brokerHost";
const char* kSettingsBrokerPort = "mqtt/brokerPort";
QString brokerHost() {
    QSettings s;
    return s.value(kSettingsBrokerHost, "172.20.32.79").toString();
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

// 영상 스트림 서버(Pi) 검증용 CA. MQTT 브로커와 같은 CA(DavoCA)로 서명하므로
// 기본값은 brokerCaPath()와 같은 파일 — 새 인증서를 발급해도 이 파일 하나만
// certs/에 있으면 되고, 배포된 클라이언트를 다시 건드릴 일이 없다.
// MQTT의 certs/ca.crt와 일부러 다른 파일명을 쓴다. 같은 파일을 공유하면(예전
// 방식) 영상 스트림 TLS를 끄려고 이 파일을 지웠을 때 MqttQtManager까지 같이
// 끊긴다 — 그쪽은 경로 문자열이 비어있을 때만 평문으로 내려가고, 파일이 없으면
// (경로는 있는데 못 읽으면) 조용히 평문으로 안 가고 아예 연결 실패로 처리하기
// 때문이다. 그래서 두 인증서 파일을 분리해 "영상만 평문으로" 끌 수 있게 한다.
// 서명한 CA는 같아도(DavoCA) 되고, 파일만 stream-ca.crt로 따로 배포하면 된다.
QString streamCaPath() {
    QSettings s;
    return s.value(QStringLiteral("stream/caCert"),
                   QCoreApplication::applicationDirPath()
                       + QStringLiteral("/certs/stream-ca.crt")).toString();
}

// 영상 서버(Pi) 인증서에 적힌 이름(CN). MQTT/scripts/generate_stream_certs.sh 의
//   openssl req -new -key streamA.key ... -subj "/CN=DaboStreamA"
// 와 반드시 같아야 한다(인덱스: 0=Pi A, 1=Pi B, MainWindow::kNumServers와 같은 크기
// 여야 하나 private static이라 여기서 직접 참조는 못 해 리터럴 2로 둔다).
const char* kStreamCommonName[2] = {"DaboStreamA", "DaboStreamB"};

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

// 이 시간 동안 새 프레임이 안 오면 소켓은 붙어있어도 "신호 끊김"으로 본다.
// 정상 송출은 목표 15fps(≈67ms 간격)라 5초면 순간 지터가 아니라 진짜 정지다.
constexpr int kChannelStaleTimeoutMs = 5000;
constexpr int kChannelHealthCheckMs  = 2000;   // 정지 판정 점검 주기

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
        sockets[i] = new QSslSocket(this);
        connect(sockets[i], &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);
        connect(sockets[i], &QTcpSocket::stateChanged, this, &MainWindow::onSocketStateChanged);
        // MQTT(mqttqtmanager.cpp)와 같은 이유의 같은 예외 처리 — 우리 서버 인증서는
        // 공인 CA가 아니라 자체 CA(DavoCA)가 서명했고, IP로 접속하는데 인증서 SAN이
        // 그 IP와 완전히 일치하지 않을 수 있어 이름불일치(HostNameMismatch)만 딱
        // 짚어 CN이 기대값(DaboStreamA/B)일 때만 눈감아준다. 그 외 에러(서명 위조,
        // 만료 등)는 그대로 연결 실패로 남는다.
        QSslSocket* sock = sockets[i];
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
    for (int i = 0; i < kNumServers; ++i) {
        if (!sockets[i]) continue;
        sockets[i]->disconnect(this);   // 이 창으로 오는 신호 차단이 먼저다
        sockets[i]->abort();            // 그 뒤에 끊어야 종료 신호가 슬롯을 타지 않는다
    }
    for (QObject* child : findChildren<QObject*>())
        child->disconnect(this);

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
        case 2: {   // 영상 검색 — 돋보기
            const qreal r = w * 0.32;
            const QPointF center(m + w * 0.38, m + w * 0.38);
            p.drawEllipse(center, r, r);
            p.drawLine(QPointF(center.x() + r * 0.72, center.y() + r * 0.72),
                       QPointF(m + w, m + w));
            break;
        }
        case 3: {   // 일일 리포트 — 문서(가로줄)
            p.drawRoundedRect(QRectF(m, m, w, w), 2.0, 2.0);
            for (int i = 0; i < 3; ++i) {
                const qreal y = m + w * 0.30 + i * w * 0.22;
                p.drawLine(QPointF(m + w * 0.2, y), QPointF(m + w * 0.8, y));
            }
            break;
        }
        case 4: {   // 입소자 관리 — 사람
            p.drawEllipse(QRectF(m + w * 0.29, m + w * 0.04, w * 0.42, w * 0.42));
            p.drawArc(QRectF(m + w * 0.06, m + w * 0.54, w * 0.88, w * 0.84), 0, 180 * 16);
            break;
        }
        default: {  // 장치 설정(5) — 카메라 바디 + 렌즈
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

// 타일 호버 툴바용 16px 선 아이콘. 유니코드 글리프(⛶ ▣ ✎ ⤓)를 쓰면 폰트에 따라
// 이모지로 대체돼 색이 튀거나 아예 두부(□)로 나온다 — 직접 그려서 모양과 색을
// 두 테마·모든 환경에서 고정한다.
QPixmap tileToolIconPixmap(int kind, const QColor& c, int px = 15)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal m = 2.0, w = px - 2 * m;
    if (kind == 0) {   // 크게 보기 — 네 모서리 꺾쇠
        const qreal a = w * 0.34;
        p.drawPolyline(QPolygonF({QPointF(m, m + a), QPointF(m, m), QPointF(m + a, m)}));
        p.drawPolyline(QPolygonF({QPointF(m + w - a, m), QPointF(m + w, m),
                                  QPointF(m + w, m + a)}));
        p.drawPolyline(QPolygonF({QPointF(m + w, m + w - a), QPointF(m + w, m + w),
                                  QPointF(m + w - a, m + w)}));
        p.drawPolyline(QPolygonF({QPointF(m + a, m + w), QPointF(m, m + w),
                                  QPointF(m, m + w - a)}));
    } else {           // 스냅샷 — 아래로 향한 화살표 + 받침
        const qreal cx = m + w / 2.0;
        p.drawLine(QPointF(cx, m), QPointF(cx, m + w * 0.62));
        p.drawPolyline(QPolygonF({QPointF(cx - w * 0.22, m + w * 0.40), QPointF(cx, m + w * 0.66),
                                  QPointF(cx + w * 0.22, m + w * 0.40)}));
        p.drawLine(QPointF(m, m + w), QPointF(m + w, m + w));
    }
    return pm;
}


}  // namespace

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
//  도움말 — 앱의 모든 기능을 설명하는 창
//
//  내용은 아래 helpTopics()에 데이터로만 모아 둔다. 기능이 바뀌면 이 목록만
//  고치면 되고, 좌측 목록 구성과 본문 렌더링(renderHelpTopic)은 그대로 둬도
//  된다 — 예전엔 switch 안에 HTML이 통째로 박혀 있어 항목 하나 추가하는 데도
//  렌더링 코드를 건드려야 했다.
// ═══════════════════════════════════════════════════════════
namespace {

// 설명 한 줄 — "용어 + 설명" 카드 하나가 된다.
struct HelpEntry {
    QString term;
    QString desc;
    bool isNew = false;      // 최근에 붙은 기능 — 카드에 NEW 배지를 단다
};
// 한 주제 안의 소제목 묶음.
struct HelpGroup {
    QString heading;         // 비우면 소제목 없이 카드만 이어 붙인다
    QVector<HelpEntry> entries;
};
struct HelpTopic {
    QString section;         // 좌측 목록의 구분 머리글(같은 값이 이어지면 한 묶음)
    QString icon;
    QString title;
    QString summary;         // 제목 아래 한 줄 요약
    QVector<HelpGroup> groups;
    QString tip;             // 하단 팁(비우면 생략)
};

const QVector<HelpTopic>& helpTopics()
{
    static const QVector<HelpTopic> topics = {
    // ── 시작 ────────────────────────────────────────────────
    {
        QStringLiteral("시작"), QStringLiteral("🏠"),
        QStringLiteral("Carenet이란"),
        QStringLiteral("Carenet은 요양원의 낙상과 침상이탈을 실시간으로 감지하고, 그 순간의 "
                       "영상과 기록을 남기는 통합 관제 프로그램입니다."),
        {
          { QStringLiteral("무엇을 하나요"), {
              { QStringLiteral("실시간 감지"),
                QStringLiteral("4채널 CCTV 영상을 서버가 계속 분석해 낙상·침상이탈을 판정하고, "
                               "발생 즉시 관제 화면과 현장 알림 노드(LED·스피커)에 알립니다.") },
              { QStringLiteral("자동 기록"),
                QStringLiteral("모든 채널을 연속 녹화하고, 이벤트가 나면 그 구간을 클립으로 따로 "
                               "남깁니다. 나중에 시각으로도, 말로 물어서도 찾을 수 있습니다.") },
              { QStringLiteral("생체신호"),
                QStringLiteral("웨어러블에서 올라오는 산소포화도·심박을 채널별로 함께 봅니다.") },
              { QStringLiteral("사람 단위 정리"),
                QStringLiteral("침대마다 입소자를 지정해 두면 알림과 기록에 이름이 붙고, "
                               "입소자별 일일 리포트로 쌓입니다.") },
          } },
          { QStringLiteral("화면 이동"), {
              { QStringLiteral("왼쪽 메뉴 6개"),
                QStringLiteral("실시간 관제 · 이벤트 기록 · 영상 검색 · 일일 리포트 · 입소자 관리 · "
                               "장치 설정. 메뉴 위 ☰ 버튼으로 접었다 펼 수 있습니다.") },
              { QStringLiteral("경보가 오면"),
                QStringLiteral("화면 가장자리에 빨간 글로우가 은은하게 켜지고, 위에서 경보 카드가 "
                               "내려오며, 해당 영상 타일이 빨간 테두리로 강조됩니다. 경보 카드의 "
                               "‘경보 해제’를 누르면 현장 사이렌·LED까지 함께 꺼집니다.") },
          } },
        },
        QStringLiteral("처음 설치했다면 <b>장치 설정 → 카메라</b>에서 CCTV를 연결하고, "
                       "<b>ROI(침대)</b>에서 침대 영역을 그리는 것부터 하세요. "
                       "침대가 없으면 낙상·침상이탈 판정이 시작되지 않습니다."),
    },
    {
        QStringLiteral("시작"), QStringLiteral("🧭"),
        QStringLiteral("화면 둘러보기"),
        QStringLiteral("어느 페이지에서나 늘 보이는 상단 헤더와 왼쪽 메뉴입니다."),
        {
          { QStringLiteral("상단 헤더"), {
              { QStringLiteral("🎤 방송"),
                QStringLiteral("한 번 누르면 방송이 시작되고, 다시 누르면 끝납니다. "
                               "현장 스피커로 목소리가 나갑니다."), true },
              { QStringLiteral("실시간 시계"),
                QStringLiteral("관제 기록의 기준이 되는 현재 시각입니다.") },
              { QStringLiteral("도움말"),
                QStringLiteral("지금 보고 있는 이 창을 엽니다.") },
              { QStringLiteral("🌙 / ☀ 테마"),
                QStringLiteral("다크(야간 관제)와 라이트(주간)를 전환합니다. 이 창도 함께 바뀝니다.") },
              { QStringLiteral("계정 · 로그아웃"),
                QStringLiteral("로그인한 사람을 보여 줍니다. 로그아웃하면 관제 화면이 닫히고 "
                               "로그인 화면으로 돌아갑니다.") },
          } },
          { QStringLiteral("경보 카드"), {
              { QStringLiteral("평상시엔 안 보입니다"),
                QStringLiteral("경보가 났을 때만 화면 위에서 내려옵니다. ‘경보 해제’ 버튼도 헤더가 "
                               "아니라 이 카드 안에 있습니다."), true },
          } },
          { QStringLiteral("왼쪽 메뉴"), {
              { QStringLiteral("☰ 접기"),
                QStringLiteral("메뉴를 아이콘만 남기고 접습니다. 영상을 넓게 볼 때 씁니다. "
                               "접힌 상태에서도 아이콘에 마우스를 올리면 이름이 뜹니다.") },
          } },
        },
        QString(),
    },

    // ── 관제 ────────────────────────────────────────────────
    {
        QStringLiteral("관제"), QStringLiteral("📺"),
        QStringLiteral("실시간 관제"),
        QStringLiteral("4채널 영상을 보면서 경보를 받고, 필요하면 그 자리에서 과거 녹화로 되돌려 "
                       "보는 기본 화면입니다."),
        {
          { QStringLiteral("왼쪽 · 리소스 패널"), {
              { QStringLiteral("카메라 · 입소자 검색"),
                QStringLiteral("채널 이름이나 입소자 이름으로 걸러 찾습니다.") },
              { QStringLiteral("수신 상태"),
                QStringLiteral("채널마다 ‘수신 중 / 신호 없음’이 표시됩니다.") },
              { QStringLiteral("접기"),
                QStringLiteral("패널을 접으면 영상 영역이 그만큼 넓어집니다.") },
          } },
          { QStringLiteral("가운데 · 영상"), {
              { QStringLiteral("레이아웃 프리셋"),
                QStringLiteral("2×2 전체 / 스포트라이트(하나를 크게 + 나머지는 작게) / 단일 채널 중에 "
                               "고릅니다."), true },
              { QStringLiteral("경보 강조"),
                QStringLiteral("낙상·침상이탈이 난 채널의 타일이 빨간 테두리로 바뀝니다.") },
              { QStringLiteral("타일 제거"),
                QStringLiteral("타일을 레이아웃에서 빼도 카메라는 그대로 녹화합니다. "
                               "‘타일 모두 표시’로 되돌립니다.") },
              { QStringLiteral("스냅샷 저장"),
                QStringLiteral("지금 화면을 PNG 이미지로 저장합니다."), true },
          } },
          { QStringLiteral("아래 · 타임라인"), {
              { QStringLiteral("라이브 ↔ 녹화 재생"),
                QStringLiteral("‘녹화 재생’으로 바꾸고 타임라인에서 시각을 고르면, 그 시각의 녹화가 "
                               "같은 자리에서 재생됩니다. ‘라이브’로 언제든 돌아옵니다."), true },
              { QStringLiteral("타임라인 읽는 법"),
                QStringLiteral("선택한 채널의 녹화가 있는 구간과 그 채널에서 난 이벤트가 함께 그려집니다. "
                               "4채널을 겹쳐 그리지 않으므로 채널을 바꾸면 타임라인도 바뀝니다.") },
              { QStringLiteral("탐색"),
                QStringLiteral("막대를 끄는 동안에는 화면이 따라오지 않고, 놓는 순간 그 시각으로 "
                               "이동합니다. 끄는 내내 새 구간을 여는 건 서버에 무리라서 그렇습니다.") },
          } },
          { QStringLiteral("오른쪽 · 웨어러블"), {
              { QStringLiteral("생체신호"),
                QStringLiteral("채널별 산소포화도·심박과 심박 추세 그래프. 정상·주의·위험에 따라 색이 "
                               "바뀌고, 신호가 끊기거나 미착용이면 회색으로 표시됩니다.") },
              { QStringLiteral("미배정"),
                QStringLiteral("그 채널에 입소자가 지정되지 않았다는 뜻입니다. "
                               "장치 설정 → 카메라 → 침대·입소자 매핑에서 지정하세요.") },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("관제"), QStringLiteral("📋"),
        QStringLiteral("이벤트 기록"),
        QStringLiteral("지금까지 난 낙상·침상이탈·생체신호 이상을 조건으로 좁혀 보고, "
                       "그 순간의 영상을 바로 재생합니다."),
        {
          { QStringLiteral("검색 조건"), {
              { QStringLiteral("즉시 반영"),
                QStringLiteral("조건을 바꾸면 바로 목록이 갱신됩니다. 따로 검색 버튼을 누르지 않습니다.") },
              { QStringLiteral("좁히는 기준"),
                QStringLiteral("병실 · 채널 · 이벤트 종류(낙상 / 침상이탈 / 생체신호 이상) · "
                               "확인 여부(전체 / 미확인만 / 확인만)."), true },
              { QStringLiteral("기간"),
                QStringLiteral("오늘 · 7일 · 30일 버튼으로 빠르게 잡거나, 시작일·종료일을 직접 고릅니다.") },
              { QStringLiteral("조건 초기화 · 새로고침"),
                QStringLiteral("초기화는 조건을 기본값으로, 새로고침은 서버 원장에서 다시 읽어 옵니다.") },
          } },
          { QStringLiteral("목록"), {
              { QStringLiteral("표에 나오는 것"),
                QStringLiteral("발생시각 · 종류 · 위치 · 입소자 · 출처 · 상태. 출처는 그 이벤트를 무엇이 "
                               "잡았는지(카메라 / 웨어러블)를 뜻합니다."), true },
              { QStringLiteral("색"),
                QStringLiteral("낙상은 빨강, 침상이탈은 주황. 상태는 미확인이 빨강, 확인이 초록입니다.") },
              { QStringLiteral("입소자가 ‘신원 미상’이면"),
                QStringLiteral("그 침대에 입소자가 지정되지 않았거나, 서버가 사람을 특정하지 못한 "
                               "경우입니다. 추적 번호는 신원이 아니라서 이름을 확정할 수 없습니다.") },
              { QStringLiteral("행이 잘려 보이면"),
                QStringLiteral("한 번에 표시하는 행 수에 상한이 있습니다. 안내가 뜨면 기간을 좀 더 "
                               "좁혀 주세요.") },
          } },
          { QStringLiteral("영상 확인"), {
              { QStringLiteral("더블클릭 → 재생"),
                QStringLiteral("행을 더블클릭하면 오른쪽 플레이어에서 그 시점 영상이 재생되고, "
                               "동시에 ‘확인’ 처리됩니다.") },
              { QStringLiteral("클립 저장"),
                QStringLiteral("재생 중인 구간을 mp4 파일로 내려받아 보관할 수 있습니다."), true },
              { QStringLiteral("‘NVR 없음’이 뜨면"),
                QStringLiteral("그 시각의 녹화가 서버에 남아 있지 않다는 뜻입니다. 저장 공간이 차서 "
                               "오래된 구간이 지워졌을 수 있습니다.") },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("관제"), QStringLiteral("🔎"),
        QStringLiteral("영상 검색"),
        QStringLiteral("시각을 몰라도 됩니다. ‘어제 저녁에 낙상 있었어?’처럼 말로 물으면 해당 기록과 "
                       "영상을 찾아 줍니다."),
        {
          { QStringLiteral("질문하기"), {
              { QStringLiteral("말로 묻습니다"),
                QStringLiteral("‘어제 저녁에 낙상 있었어?’, ‘이번 주에 침대에서 나간 적 있어?’, "
                               "‘오늘 새벽에 무슨 일 있었어?’ 같은 문장을 그대로 적고 검색을 누릅니다."), true },
              { QStringLiteral("예시 질문"),
                QStringLiteral("아래 예시를 누르면 질문칸에 바로 채워집니다. 어떤 식으로 물으면 되는지 "
                               "감을 잡을 때 쓰세요.") },
              { QStringLiteral("채널 한정"),
                QStringLiteral("기본값은 전체 채널입니다. 특정 병상만 보고 싶을 때만 채널을 고르세요.") },
              { QStringLiteral("초기화"),
                QStringLiteral("질문과 조건, 결과를 한 번에 지웁니다.") },
          } },
          { QStringLiteral("결과 보기"), {
              { QStringLiteral("결과 목록"),
                QStringLiteral("찾은 기록이 ‘시각 · 채널 · 종류 · 입소자’ 형태로 나열됩니다.") },
              { QStringLiteral("눌러서 바로 재생"),
                QStringLiteral("결과를 누르면 이 페이지 안 오른쪽 재생기에서 바로 틀어 줍니다. "
                               "▶/⏸와 탐색 막대로 구간을 넘겨 볼 수 있습니다. 검색을 이어가려고 "
                               "다른 페이지로 튕겨 나가지 않습니다."), true },
              { QStringLiteral("‘저장된 클립이 없는 기록입니다’"),
                QStringLiteral("기록은 남아 있지만 영상 파일이 없다는 뜻입니다(보관 기간 경과 등).") },
          } },
        },
        QStringLiteral("이 기능은 <b>영상 서버에 연결되어 있어야</b> 동작합니다. "
                       "‘영상 서버에 연결되어 있지 않습니다’가 뜨면 서버 연결부터 확인하세요."),
    },

    // ── 기록·관리 ────────────────────────────────────────────
    {
        QStringLiteral("기록 · 관리"), QStringLiteral("📈"),
        QStringLiteral("일일 리포트"),
        QStringLiteral("하루 동안 한 입소자가 어떻게 지냈는지를 숫자와 그래프로 정리해 봅니다."),
        {
          { QStringLiteral("무엇을 고르나"), {
              { QStringLiteral("날짜"),
                QStringLiteral("왼쪽 달력에서 날짜를 고릅니다. 자료가 없는 미래 날짜는 선택되지 않고, "
                               "‘오늘’ 버튼으로 언제든 돌아옵니다.") },
              { QStringLiteral("입소자"),
                QStringLiteral("위쪽 이름 탭으로 사람을 바꿉니다. 리포트는 ‘날짜 한 개 + 입소자 한 명’ "
                               "단위입니다.") },
          } },
          { QStringLiteral("지표"), {
              { QStringLiteral("누워있는 시간"),
                QStringLiteral("침대 ROI 안에 누워 있던 시간의 합입니다.") },
              { QStringLiteral("활동량"),
                QStringLiteral("웨어러블 만보기 기준 걸음 수입니다.") },
              { QStringLiteral("케어시간"),
                QStringLiteral("요양보호사가 곁에 머문 시간으로 기록된 값입니다.") },
              { QStringLiteral("이벤트"),
                QStringLiteral("그날 그 사람에게 난 낙상·침상이탈 횟수입니다.") },
              { QStringLiteral("시간별 활동량"),
                QStringLiteral("하루를 시간대로 쪼개 활동량을 막대로 보여 줍니다. 밤에 유난히 "
                               "움직임이 많았던 시간대를 찾을 때 유용합니다.") },
          } },
          { QStringLiteral("내보내기"), {
              { QStringLiteral("PDF 내보내기"),
                QStringLiteral("보고 있는 리포트를 그대로 PDF로 저장합니다. 보호자 설명이나 "
                               "인수인계 자료로 씁니다."), true },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("기록 · 관리"), QStringLiteral("👥"),
        QStringLiteral("입소자 관리"),
        QStringLiteral("입소자를 등록·수정·퇴원 처리하고, 위험도와 채널 배정을 관리합니다."),
        {
          { QStringLiteral("한눈에"), {
              { QStringLiteral("상단 요약"),
                QStringLiteral("재원 인원, 위험도 상/중/하 분포, 채널 배정 수를 보여 줍니다.") },
          } },
          { QStringLiteral("찾기"), {
              { QStringLiteral("재원 / 전체 / 퇴원"),
                QStringLiteral("왼쪽 목록을 상태별로 전환합니다.") },
              { QStringLiteral("🔍 이름 검색"),
                QStringLiteral("이름 일부만 입력해도 걸러집니다.") },
              { QStringLiteral("행 왼쪽 색 띠"),
                QStringLiteral("위험도입니다 — 상은 빨강, 중은 주황, 하는 초록.") },
          } },
          { QStringLiteral("편집"), {
              { QStringLiteral("목록 → 상세"),
                QStringLiteral("행을 클릭하면 오른쪽에서 바로 편집합니다. 팝업이 뜨지 않습니다.") },
              { QStringLiteral("＋ 신규 등록 · 저장"),
                QStringLiteral("새 입소자를 추가하거나 고친 내용을 저장합니다.") },
              { QStringLiteral("퇴원 처리"),
                QStringLiteral("퇴원시키거나 다시 재입원시킵니다. 바뀐 내역은 입원 이력에 남습니다.") },
          } },
        },
        QStringLiteral("여기서 등록한 입소자를 <b>장치 설정 → 카메라 → 침대·입소자 매핑</b>에서 "
                       "침대에 지정해야, 경보와 기록에 이름이 붙습니다."),
    },

    // ── 설정 ────────────────────────────────────────────────
    {
        QStringLiteral("설정"), QStringLiteral("🎥"),
        QStringLiteral("장치 설정 · 카메라"),
        QStringLiteral("CCTV를 연결하고, 침대 영역을 그리고, 화질과 초점을 원격으로 맞춥니다."),
        {
          { QStringLiteral("공통"), {
              { QStringLiteral("[카메라] / [알림] 전환"),
                QStringLiteral("위쪽에서 카메라 설정과 알림 노드 설정을 오갑니다.") },
              { QStringLiteral("CH1~4 채널 레일"),
                QStringLiteral("위에서 채널을 고르면 아래 설정과 오른쪽 영상이 그 채널로 함께 묶입니다. "
                               "각 채널에 연결 상태와 지정된 침대 수가 배지로 표시됩니다.") },
          } },
          { QStringLiteral("연결"), {
              { QStringLiteral("직접 입력"),
                QStringLiteral("CCTV IP · 계정 · 비밀번호를 넣고 연결합니다. 포트(554)와 프로파일은 "
                               "고정이라 입력하지 않습니다.") },
              { QStringLiteral("🔍 같은 망 카메라 검색"),
                QStringLiteral("같은 망의 ONVIF 카메라를 자동으로 찾아 모델·IP·MAC을 보여 줍니다. "
                               "행을 클릭하면 IP가 자동으로 채워집니다."), true },
              { QStringLiteral("다른 대역도 찾고 싶다면"),
                QStringLiteral("IP칸에 그 대역의 주소를 하나 적고 검색하면 그 대역까지 함께 훑습니다."), true },
              { QStringLiteral("연결하는 주체는 서버입니다"),
                QStringLiteral("Carenet이 카메라에 직접 붙는 게 아니라, IP를 서버(라즈베리파이)로 "
                               "보내면 서버가 RTSP를 엽니다. 그래서 <b>서버가 닿을 수 있는 대역</b>의 "
                               "카메라여야 영상이 나옵니다.") },
              { QStringLiteral("전체 해제"),
                QStringLiteral("4채널 연결을 한꺼번에 끊고 서버를 대기 상태로 되돌립니다.") },
          } },
          { QStringLiteral("ROI(침대)"), {
              { QStringLiteral("그리는 순서"),
                QStringLiteral("‘침대 추가’를 누르고 → 오른쪽 영상 위를 클릭해 모서리를 찍고 → "
                               "더블클릭(또는 우클릭)으로 완료합니다.") },
              { QStringLiteral("여러 개 그릴 수 있습니다"),
                QStringLiteral("한 채널에 침대를 최대 8개까지 지정할 수 있고, 각 침대가 낙상·침상이탈 "
                               "판정의 기준이 됩니다."), true },
              { QStringLiteral("영상에 표시 · 침대 제거"),
                QStringLiteral("그려 둔 영역을 화면에 겹쳐 볼지 끄고 켭니다. 잘못 그렸으면 제거 후 "
                               "다시 그리세요.") },
          } },
          { QStringLiteral("침대 · 입소자 매핑"), {
              { QStringLiteral("이름 붙이기"),
                QStringLiteral("침대 목록에서 그 침대의 입소자를 지정하면, 그 침대에서 감지된 사람에게 "
                               "이름이 붙어 ‘침대 2 김복순’처럼 알립니다.") },
              { QStringLiteral("현장 LED에도 나옵니다"),
                QStringLiteral("알림 노드 LED에 입소자 이름이 가운데 글자를 가린 형태로 표시됩니다."), true },
              { QStringLiteral("‘신원 미상’"),
                QStringLiteral("서버가 사람을 특정하지 못하면 이렇게 알립니다. 추적 번호는 신원이 "
                               "아니어서 이름을 확정할 수 없습니다.") },
          } },
          { QStringLiteral("이미지"), {
              { QStringLiteral("밝기 · 대비 · 채도"),
                QStringLiteral("슬라이더로 맞춘 뒤 ‘적용’을 누르면 카메라에 바로 반영됩니다. "
                               "‘초기화’로 되돌립니다."), true },
              { QStringLiteral("적용 전 / 적용 후 비교"),
                QStringLiteral("오른쪽에서 원본과 적용 결과를 나란히 비교할 수 있습니다.") },
              { QStringLiteral("초점"),
                QStringLiteral("실시간 영상에서 원하는 지점을 클릭하면 그 지점에 초점을 맞춥니다. "
                               "‘전체 자동초점’으로 카메라에 다시 맡길 수도 있습니다."), true },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("설정"), QStringLiteral("🔔"),
        QStringLiteral("장치 설정 · 알림 노드"),
        QStringLiteral("병실에 달린 LED·스피커 노드의 밝기와 음량을 원격으로 맞춥니다."),
        {
          { QStringLiteral("대상 고르기"), {
              { QStringLiteral("대상 노드"),
                QStringLiteral("설정할 알림 노드를 고릅니다. 아직 응답이 없으면 ‘상태 미확인’으로 "
                               "표시됩니다.") },
              { QStringLiteral("LED 미리보기 (64×32)"),
                QStringLiteral("실제 LED와 같은 해상도로, 지금 밝기가 어떻게 보일지 화면에서 "
                               "미리 확인합니다."), true },
          } },
          { QStringLiteral("값 맞추기"), {
              { QStringLiteral("LED 밝기 · 스피커 음량"),
                QStringLiteral("낮에는 밝게, 밤에는 눈부시지 않게 — 병실 상황에 맞춰 조절합니다.") },
              { QStringLiteral("테스트"),
                QStringLiteral("저장하지 않고 지금 값으로 현장 LED에 문구를 한 번 띄우고 짧은 소리를 "
                               "냅니다. 실제 낙상 안내 음성이 나가지는 않습니다."), true },
              { QStringLiteral("적용"),
                QStringLiteral("지금 값을 노드에 바로 반영하고 저장합니다. 마지막으로 적용한 시각이 "
                               "아래에 남습니다.") },
          } },
          { QStringLiteral("현장에서는"), {
              { QStringLiteral("낙상이 나면"),
                QStringLiteral("LED에 입소자 이름과 호실이 뜨고 스피커로 안내가 나갑니다. 호실을 "
                               "모르면 호실 없이 안내합니다."), true },
          } },
        },
        QString(),
    },

    // ── 도움 ────────────────────────────────────────────────
    {
        QStringLiteral("도움"), QStringLiteral("🛠"),
        QStringLiteral("문제 해결"),
        QStringLiteral("자주 막히는 지점과 확인 순서입니다."),
        {
          { QStringLiteral("영상"), {
              { QStringLiteral("영상이 안 나옵니다"),
                QStringLiteral("① 상단에 서버 연결이 살아 있는지 ② 장치 설정 → 카메라에서 그 채널이 "
                               "연결되어 있는지 ③ CCTV 계정·비밀번호가 맞는지 순서로 확인하세요.") },
              { QStringLiteral("일부 채널만 안 나옵니다"),
                QStringLiteral("채널은 두 대의 서버가 나눠 맡습니다(CH1·2 / CH3·4). 한쪽 서버만 "
                               "끊기면 두 채널만 검게 남습니다.") },
          } },
          { QStringLiteral("카메라 검색"), {
              { QStringLiteral("검색해도 아무것도 안 뜹니다"),
                QStringLiteral("① 관제 PC와 카메라가 같은 공유기·스위치에 물려 있는지 ② 카메라의 "
                               "ONVIF 검색이 켜져 있는지 ③ PC 방화벽이 Carenet의 UDP 수신을 막고 있지 "
                               "않은지 확인하세요.") },
              { QStringLiteral("다른 대역에 있습니다"),
                QStringLiteral("IP칸에 그 대역의 주소를 하나 적고 다시 검색하면 그 대역까지 훑습니다. "
                               "다만 찾더라도 서버가 그 대역에 닿아야 영상이 열립니다."), true },
          } },
          { QStringLiteral("경보"), {
              { QStringLiteral("낙상 알림이 오지 않습니다"),
                QStringLiteral("그 채널에 침대 ROI가 그려져 있는지 먼저 보세요. 침대가 없으면 판정 "
                               "자체가 시작되지 않습니다.") },
              { QStringLiteral("알림에 이름이 안 뜹니다"),
                QStringLiteral("장치 설정 → 카메라 → 침대·입소자 매핑에서 그 침대에 입소자를 "
                               "지정하세요.") },
              { QStringLiteral("경보음이 계속 납니다"),
                QStringLiteral("화면 위 경보 카드의 ‘경보 해제’를 누르면 현장 사이렌·LED까지 함께 "
                               "꺼집니다.") },
          } },
        },
        QStringLiteral("그래도 해결되지 않으면 발생 시각과 채널을 적어 두세요 — "
                       "<b>영상 검색</b>에서 그 시각의 영상을 찾아 원인을 짚을 수 있습니다."),
    },
    };
    return topics;
}

}  // namespace

void MainWindow::onHelpClicked()
{
    if (!helpDialog) {
        helpDialog = new QDialog(this);
        helpDialog->setObjectName("panel");
        helpDialog->setWindowTitle(QStringLiteral("Carenet 도움말"));
        helpDialog->resize(940, 700);
        helpDialog->setMinimumSize(700, 480);
        enableDarkTitleBar(helpDialog);
        auto* h = new QHBoxLayout(helpDialog);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        // ── 좌: 제목 + 검색 + 주제 목록 ──
        auto* side = new QFrame();
        side->setObjectName(QStringLiteral("helpSide"));
        side->setFixedWidth(236);
        auto* sv = new QVBoxLayout(side);
        sv->setContentsMargins(14, 16, 14, 12);
        sv->setSpacing(9);

        auto* sideTitle = new QLabel(QStringLiteral("Carenet 도움말"));
        sideTitle->setObjectName(QStringLiteral("helpSideTitle"));
        sv->addWidget(sideTitle);

        auto* search = new QLineEdit();
        search->setObjectName(QStringLiteral("helpSearch"));
        search->setPlaceholderText(QStringLiteral("🔍  기능 검색"));
        search->setClearButtonEnabled(true);
        sv->addWidget(search);

        helpList = new QListWidget();
        helpList->setObjectName(QStringLiteral("helpList"));
        helpList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sv->addWidget(helpList, 1);
        h->addWidget(side);

        // 주제를 section별로 묶어 채운다 — 머리글 행은 선택되지 않게 두고,
        // 실제 주제 행에만 UserRole로 주제 번호를 심는다(행 번호 ≠ 주제 번호).
        const QVector<HelpTopic>& topics = helpTopics();
        QString lastSection;
        for (int i = 0; i < topics.size(); ++i) {
            const HelpTopic& tp = topics.at(i);
            if (tp.section != lastSection) {
                lastSection = tp.section;
                auto* head = new QListWidgetItem(tp.section, helpList);
                head->setData(Qt::UserRole, -1);
                head->setFlags(Qt::NoItemFlags);          // 선택·호버 대상에서 제외
                head->setForeground(QColor(QString::fromLatin1(kAccent)));
                QFont hf = head->font();
                hf.setPointSizeF(hf.pointSizeF() - 1.0);
                hf.setBold(true);
                head->setFont(hf);
            }
            auto* it = new QListWidgetItem(
                QStringLiteral("%1   %2").arg(tp.icon, tp.title), helpList);
            it->setData(Qt::UserRole, i);
            // 검색이 훑을 건초더미 — 제목뿐 아니라 본문 용어·설명까지 넣어야
            // "스냅샷", "PDF"처럼 본문에만 있는 말로도 주제를 찾을 수 있다.
            QString hay = tp.title + QLatin1Char(' ') + tp.summary;
            for (const HelpGroup& g : tp.groups) {
                hay += QLatin1Char(' ') + g.heading;
                for (const HelpEntry& e : g.entries)
                    hay += QLatin1Char(' ') + e.term + QLatin1Char(' ') + e.desc;
            }
            it->setData(Qt::UserRole + 1, hay);
        }

        // ── 우: 본문 ──
        helpBrowser = new QTextBrowser();
        helpBrowser->setObjectName(QStringLiteral("helpBrowser"));
        helpBrowser->setOpenExternalLinks(false);
        h->addWidget(helpBrowser, 1);

        connect(helpList, &QListWidget::currentRowChanged, this,
                &MainWindow::renderHelpTopic);

        connect(search, &QLineEdit::textChanged, this, [this](const QString& q) {
            const QString needle = q.trimmed();
            for (int i = 0; i < helpList->count(); ++i) {
                QListWidgetItem* it = helpList->item(i);
                if (it->data(Qt::UserRole).toInt() < 0) continue;   // 머리글은 뒤에서
                it->setHidden(!needle.isEmpty() &&
                              !it->data(Qt::UserRole + 1).toString()
                                   .contains(needle, Qt::CaseInsensitive));
            }
            // 머리글은 자기 묶음에 남은 주제가 하나도 없을 때만 숨긴다.
            for (int i = 0; i < helpList->count(); ++i) {
                QListWidgetItem* head = helpList->item(i);
                if (head->data(Qt::UserRole).toInt() >= 0) continue;
                bool any = false;
                for (int j = i + 1; j < helpList->count(); ++j) {
                    QListWidgetItem* n = helpList->item(j);
                    if (n->data(Qt::UserRole).toInt() < 0) break;   // 다음 묶음 시작
                    if (!n->isHidden()) { any = true; break; }
                }
                head->setHidden(!any);
            }
        });

        helpList->setCurrentRow(1);   // 0은 머리글 — 첫 실제 주제는 1행
    }
    renderHelpTopic(helpList ? helpList->currentRow() : 1);  // 현재 테마 색으로 갱신
    helpDialog->show();
    helpDialog->raise();
    helpDialog->activateWindow();
}

// 선택된 도움말 주제를 현재 테마 색으로 렌더한다.
//
// QTextBrowser가 이해하는 건 Qt 리치텍스트(HTML 부분집합)라 flex도 border-radius도
// 없다. 대신 표 셀의 배경색·패딩은 확실히 먹으므로, 카드는 전부 "색 띠 칸 + 내용 칸"
// 2칸짜리 표로 만든다. 색은 매번 현재 팔레트에서 새로 읽어 테마 토글에 따라온다.
void MainWindow::renderHelpTopic(int row)
{
    if (!helpBrowser) return;

    // 좌측 목록엔 머리글 행이 섞여 있어 행 번호가 곧 주제 번호가 아니다.
    // 머리글이 넘어오면(또는 검색으로 선택이 풀리면) 보고 있던 주제를 유지한다.
    int idx = helpTopicShown_;
    if (helpList) {
        if (const QListWidgetItem* it = helpList->item(row)) {
            const int t = it->data(Qt::UserRole).toInt();
            if (t >= 0) idx = t;
        }
    }
    const QVector<HelpTopic>& topics = helpTopics();
    if (topics.isEmpty()) return;
    idx = qBound(0, idx, topics.size() - 1);
    helpTopicShown_ = idx;
    const HelpTopic& tp = topics.at(idx);

    const QString A  = QString::fromLatin1(kAccent);
    const QString T  = QString::fromLatin1(kTextMain);
    const QString S  = QString::fromLatin1(kTextSub);
    const QString BD = QString::fromLatin1(kBorder);
    const QString C  = QString::fromLatin1(kCard);
    const QString OA = QString::fromLatin1(kOnAccent);
    const QString W  = QString::fromLatin1(kWarn);

    // 색 띠(왼쪽 3px) + 내용 카드.
    auto card = [&](const QString& bar, const QString& head, const QString& body) {
        return QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0' style='margin:0 0 7px;'>"
            "<tr><td width='3' bgcolor='%1'></td>"
            "<td bgcolor='%2' style='padding:9px 14px;'>"
            "<div style='font-size:13px; font-weight:700; color:%3;'>%4</div>"
            "<div style='font-size:13px; color:%5; line-height:152%;'>%6</div>"
            "</td></tr></table>").arg(bar, C, T, head, S, body);
    };

    QString html = QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='0'><tr>"
        "<td width='46' valign='top'><span style='font-size:28px;'>%1</span></td>"
        "<td valign='top'>"
        "<div style='font-size:21px; font-weight:800; color:%2;'>%3</div>"
        "<div style='font-size:13px; color:%4; line-height:152%;'>%5</div>"
        "</td></tr></table>"
        // 구분선은 <hr>로만 둔다 — 높이 1px짜리 표 행은 Qt가 글자 높이만큼
        // 부풀려 두꺼운 띄로 그려버리고, div의 border-top은 아예 무시된다.
        "<hr style='border:none; border-top:1px solid %6;'>")
        .arg(tp.icon, T, tp.title, S, tp.summary, BD);

    for (const HelpGroup& g : tp.groups) {
        if (!g.heading.isEmpty())
            html += QStringLiteral(
                "<div style='color:%1; font-size:12px; font-weight:800; "
                "margin:15px 0 7px; letter-spacing:1px;'>%2</div>").arg(A, g.heading);
        for (const HelpEntry& e : g.entries) {
            QString head = e.term;
            if (e.isNew)
                head += QStringLiteral(
                    "&nbsp;&nbsp;<span style='background-color:%1; color:%2; "
                    "font-size:10px; font-weight:800;'>&nbsp;NEW&nbsp;</span>").arg(A, OA);
            html += card(A, head, e.desc);
        }
    }
    if (!tp.tip.isEmpty())
        html += QStringLiteral("<div style='margin-top:9px;'></div>")
              + card(W, QStringLiteral("💡 팁"), tp.tip);

    helpBrowser->setHtml(
        QStringLiteral(
            "<div style='font-family:\"Segoe UI\",\"맑은 고딕\",\"Malgun Gothic\","
            "\"Apple SD Gothic Neo\",\"Noto Sans CJK KR\",\"Noto Sans KR\",sans-serif; "
            "font-size:13px; color:%1;'>%2</div>").arg(T, html));
    // 배경/여백은 base.qss의 QTextBrowser#helpBrowser 규칙이 담당한다.
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

// ═════════════════════════════════════════════════════════
//  좌측 리소스 패널 — Wisenet Viewer의 Resource 패널에 대응.
//  네비 레일이 "어느 화면을 볼까"를 고른다면, 이 패널은 관제 화면 안에서
//  "어느 카메라를 다룰까"를 고른다 — 둘을 합치면 카메라가 늘어날 때
//  페이지 전환과 카메라 선택이 같은 줄에 섞여 길어진다.
// ═════════════════════════════════════════════════════════
QWidget* MainWindow::buildResourcePanel()
{
    resourcePanel_ = new QFrame();
    resourcePanel_->setObjectName("resourcePanel");

    auto* outer = new QVBoxLayout(resourcePanel_);
    outer->setContentsMargins(10, 12, 10, 12);
    outer->setSpacing(8);

    // 헤더 — "리소스" + 접기 토글
    auto* head = new QHBoxLayout();
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(6);
    resourceHead_ = new QLabel(QStringLiteral("리소스"));
    resourceHead_->setObjectName("resourceHead");
    head->addWidget(resourceHead_, 1);
    resourceToggle_ = new QPushButton(QStringLiteral("‹"));
    resourceToggle_->setObjectName("resourceToggle");
    resourceToggle_->setCursor(Qt::PointingHandCursor);
    resourceToggle_->setFixedSize(22, 22);
    resourceToggle_->setToolTip(QStringLiteral("리소스 패널 접기/펼치기"));
    connect(resourceToggle_, &QPushButton::clicked, this,
            [this] { setResourceCollapsed(!resourceCollapsed_); });
    head->addWidget(resourceToggle_);
    outer->addLayout(head);

    // 접었을 때 통째로 숨길 부분(검색 + 트리). 헤더만 남아 다시 펼 수 있다.
    resourceBody_ = new QWidget();
    auto* body = new QVBoxLayout(resourceBody_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);

    resourceSearch_ = new QLineEdit();
    resourceSearch_->setObjectName("resourceSearch");
    resourceSearch_->setPlaceholderText(QStringLiteral("카메라 · 입소자 검색"));
    resourceSearch_->setClearButtonEnabled(true);
    connect(resourceSearch_, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshResourceTree(); });
    body->addWidget(resourceSearch_);

    resourceTree_ = new QTreeWidget();
    resourceTree_->setObjectName("resourceTree");
    resourceTree_->setHeaderHidden(true);
    resourceTree_->setIndentation(14);
    resourceTree_->setRootIsDecorated(true);
    resourceTree_->setUniformRowHeights(true);
    resourceTree_->setFrameShape(QFrame::NoFrame);
    resourceTree_->setIconSize(QSize(16, 16));
    resourceTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    body->addWidget(resourceTree_, 1);

    auto* root = new QTreeWidgetItem(resourceTree_,
                                     QStringList(QStringLiteral("Root")));
    root->setData(0, Qt::UserRole, QStringLiteral("root"));
    rebuildResourceRooms();

    // ── 레이아웃 프리셋 ──
    auto* layoutRoot = new QTreeWidgetItem(resourceTree_,
                                           QStringList(QStringLiteral("레이아웃")));
    layoutRoot->setData(0, Qt::UserRole, QStringLiteral("layoutRoot"));
    const QString layoutNames[3] = {QStringLiteral("2×2 전체"),
                                    QStringLiteral("스포트라이트"),
                                    QStringLiteral("단일 채널")};
    for (int i = 0; i < 3; ++i) {
        auto* it = new QTreeWidgetItem(layoutRoot, QStringList(layoutNames[i]));
        it->setData(0, Qt::UserRole, QStringLiteral("layout"));
        it->setData(0, Qt::UserRole + 1, i);
        it->setIcon(0, QIcon(navIconPixmap(0, QColor(QString::fromLatin1(kTextSub)), 16)));
    }
    layoutRoot->setExpanded(true);

    connect(resourceTree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                const QString kind = item->data(0, Qt::UserRole).toString();
                // 방 그룹을 누르면 영상월이 그 방으로 바뀐다. 빈 방의 채널 자리를
                // 눌러도 같은 뜻으로 받는다 — 사람은 그룹이 아니라 눈에 보이는
                // 채널 줄을 누른다.
                if (kind == QLatin1String("group") || kind == QLatin1String("groupEmpty")) {
                    selectRoom(item->data(0, Qt::UserRole + 1).toInt());
                    return;
                }
                if (kind == QLatin1String("camEmpty")) {
                    QTreeWidgetItem* g = item->parent();
                    if (g) selectRoom(g->data(0, Qt::UserRole + 1).toInt());
                    return;
                }
                if (kind == QLatin1String("cam")) {
                    selectRoom(0);   // 실카메라 방으로 돌아온 뒤 그 채널을 고른다
                    selectChannel(item->data(0, Qt::UserRole + 1).toInt());
                } else if (kind == QLatin1String("layout")) {
                    const int i = item->data(0, Qt::UserRole + 1).toInt();
                    setGridLayout(i == 0   ? GridLayout::Quad
                                  : i == 1 ? GridLayout::Spotlight
                                           : GridLayout::Single);
                }
            });
    // 더블클릭 = 그 카메라만 크게 — Wisenet에서 트리 항목을 더블클릭한 것과 같은 감각.
    connect(resourceTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item || item->data(0, Qt::UserRole).toString() != QLatin1String("cam"))
                    return;
                selectRoom(0);
                selectChannel(item->data(0, Qt::UserRole + 1).toInt());
                setGridLayout(GridLayout::Single);
            });

    outer->addWidget(resourceBody_, 1);

    // 접었을 때 보이는 세로 칩 레일 — 채널 번호 + 연결 상태색. 누르면 그 채널 선택.
    resourceRail_ = new QWidget();
    auto* rv = new QVBoxLayout(resourceRail_);
    rv->setContentsMargins(0, 6, 0, 0);
    rv->setSpacing(4);
    for (int ch = 0; ch < 4; ++ch) {
        auto* chip = new QPushButton(QString::number(ch + 1));
        chip->setObjectName("resChip");
        chip->setCheckable(true);
        chip->setAutoExclusive(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFixedSize(30, 26);
        connect(chip, &QPushButton::clicked, this, [this, ch] { selectChannel(ch); });
        resChipBtns_[ch] = chip;
        rv->addWidget(chip, 0, Qt::AlignHCenter);
    }
    rv->addStretch();
    resourceRail_->hide();
    outer->addWidget(resourceRail_, 1);

    QSettings st;
    // 기본은 접힘 — 좌측에 네비 레일과 이 패널이 나란히 펼쳐지면 크롬이
    // 화면 폭의 절반 가까이를 먹는다. 필요할 때만 펴서 쓴다.
    //
    // 키 이름을 _v2로 바꾼 이유: setResourceCollapsed()가 값을 되쓰기 때문에
    // 예전 키(ui/resource_collapsed)는 이미 모든 기존 사용자에게 false로 남아 있다.
    // 같은 키를 그대로 쓰면 "기본 접힘"이 아무에게도 적용되지 않는다. 새 키로 한 번
    // 초기화하고, 이후 사용자가 편 선택은 그대로 기억한다.
    setResourceCollapsed(st.value(QStringLiteral("ui/resource_collapsed_v2"), true).toBool());
    refreshResourceTree();
    return resourcePanel_;
}

// 트리의 표시만 다시 만든다(항목 자체는 재생성하지 않는다).
// 다시 만들면 펼침 상태와 스크롤 위치가 매번 초기화돼 손으로 다시 열어야 한다.
// Root 아래 방 그룹을 처음부터 다시 만든다. 방을 추가·삭제하면 항목 자체가
// 달라지므로 refreshResourceTree(표시 갱신)로는 부족하고 이 함수가 필요하다.
void MainWindow::rebuildResourceRooms()
{
    if (!resourceTree_) return;
    QTreeWidgetItem* root = resourceTree_->topLevelItem(0);
    if (!root) return;
    qDeleteAll(root->takeChildren());
    for (int ch = 0; ch < 4; ++ch) camItems_[ch] = nullptr;

    // ── Root > 방 그룹 > 채널 4개 ──
    // 0번 방만 실제 카메라를 갖는다. 나머지 방은 채널 4칸을 회색으로 비워 두는데,
    // 눌러도 아무 일도 일어나지 않는다(연결된 영상이 없으니 고를 것도 없다).
    const QStringList rooms = roomNames();
    for (int r = 0; r < rooms.size(); ++r) {
        auto* group = new QTreeWidgetItem(root);
        group->setData(0, Qt::UserRole,
                       r == 0 ? QStringLiteral("group") : QStringLiteral("groupEmpty"));
        group->setData(0, Qt::UserRole + 1, r);
        group->setText(0, rooms.at(r));
        if (r == 0) {
            for (int ch = 0; ch < 4; ++ch) {
                camItems_[ch] = new QTreeWidgetItem(group);
                camItems_[ch]->setData(0, Qt::UserRole, QStringLiteral("cam"));
                camItems_[ch]->setData(0, Qt::UserRole + 1, ch);
            }
        } else {
            group->setToolTip(0, QStringLiteral("카메라가 아직 연결되지 않은 방입니다"));
            for (int ch = 0; ch < 4; ++ch) {
                auto* slot = new QTreeWidgetItem(group);
                slot->setData(0, Qt::UserRole, QStringLiteral("camEmpty"));
                slot->setText(0, QStringLiteral("채널 %1").arg(ch + 1));
                slot->setToolTip(0, QStringLiteral("카메라 미연결 — 빈 자리입니다"));
            }
        }
        group->setExpanded(true);
    }
    root->setExpanded(true);
    root->setExpanded(true);
}

// 트리에서 방을 고르면 영상월을 그 방으로 전환한다.
//
// 방을 바꿔도 서버에는 아무 것도 보내지 않는다 — 101호 카메라는 계속 스트리밍하고
// 낙상 감지도 계속 돈다. 바뀌는 건 "이 화면이 무엇을 보여 주는가"뿐이다. 그래서
// 102호를 보다가 101호로 돌아오면 끊긴 자리에서 이어지는 게 아니라 지금 실시간이
// 바로 뜬다(카메라 해제와 결정적으로 다른 점).
void MainWindow::selectRoom(int room)
{
    const int count = roomNames().size();
    room = qBound(0, room, count - 1);
    if (room == selectedRoom_) return;

    // 실카메라 방을 떠나기 직전에 접속 폼을 보관한다 — applyRoomView가 비우기 때문에
    // 여기서 안 챙기면 102호에 한 번 들렀다 오는 것만으로 입력값이 날아간다.
    if (selectedRoom_ == 0 && room != 0) {
        camFormBackup_[0] = camIpEdit   ? camIpEdit->text()   : QString();
        camFormBackup_[1] = camUserEdit ? camUserEdit->text() : QString();
        camFormBackup_[2] = camPwEdit   ? camPwEdit->text()   : QString();
    }

    selectedRoom_ = room;
    applyRoomView();

    if (room == 0) {
        if (camIpEdit)   camIpEdit->setText(camFormBackup_[0]);
        if (camUserEdit) camUserEdit->setText(camFormBackup_[1]);
        if (camPwEdit)   camPwEdit->setText(camFormBackup_[2]);
        if (discoveryStatus)
            discoveryStatus->setText(QStringLiteral(
                "‘검색’을 누르면 같은 망의 카메라가 아래에 나타납니다. 행을 클릭하면 IP가 채워져요."));
        for (int ch = 0; ch < 4; ++ch) refreshRoiZones(ch);   // 침대 오버레이 복구
    }
}

// selectedRoom_에 맞춰 타일·바이탈·트리를 한 번에 맞춘다.
// videoSuppressed_는 이제 "해제했다"가 아니라 "지금 이 화면에 그리면 안 된다"는
// 파생값이다 — (보고 있는 방이 실카메라 방인가) × (그 채널을 해제하지 않았는가).
//
// 여기서 cameraActive_를 쓰지 않는 게 중요하다. 그 플래그는 "해제 버튼을 눌러도
// 되는가"를 나타낼 뿐이고, 서버는 Qt 재시작과 무관하게 계속 스트리밍한다 —
// active_mask가 비어 있어도 프레임은 들어오고 예전엔 그대로 화면에 떴다.
// 조건에 끼워 넣으면 그 경우에 영상이 통째로 사라진다.
void MainWindow::applyRoomView()
{
    const bool liveRoom = (selectedRoom_ == 0);
    for (int ch = 0; ch < 4; ++ch) {
        // draw  = 들어온 프레임을 그릴 것인가 (실제 차단은 이 값 하나로 한다)
        // known = 타일에 "연결됨"으로 적을 것인가. 둘을 나눈 이유: 서버가 이미
        //         스트리밍 중인데 active_mask만 비어 있는 경우가 있고, 그때도
        //         영상은 떠야 한다. 프레임이 도착하면 VideoView가 스스로
        //         cameraConnected_를 켜므로 라벨은 자연히 따라온다.
        const bool draw  = liveRoom && !videoCleared_[ch];
        const bool known = draw && cameraActive_[ch];
        videoSuppressed_[ch] = !draw;
        if (auto* v = channelViews[ch]) {
            // setCameraConnected(false)가 직전 프레임까지 지운다 — 방을 옮겼는데
            // 이전 방 화면이 정지영상으로 남아 있으면 그게 제일 위험한 오해다.
            v->setCameraConnected(known);
            if (!draw) v->setLive(false);
            // 침대 오버레이는 101호의 것이다 — 빈 방 타일에 남으면 그 방에
            // 침대가 있는 것처럼 보인다. 돌아올 때는 selectRoom()이 되살린다.
            if (!liveRoom) v->setZones({});
            v->setDisplayName(tileDisplayName(ch));
            // 배지는 "빈 방으로 나갈 때"만 지운다. 돌아올 때도 지우면 경보 경로가
            // setAlert(true) → setVideoFocus → selectRoom(0) 순서라서 방금 켠
            // 배지를 스스로 꺼 버린다.
            if (!liveRoom) v->setAlert(false);
        }
        if (!draw) {
            lastFramePix_[ch] = QPixmap();
            if (camThumbs[ch]) camThumbs[ch]->clearFrame();
        }
    }
    // 설정 화면(스테이지 영상)도 같은 방을 따른다.
    if (roiEditorView) {
        const bool stage = liveRoom && cameraActive_[roiEditChannel];
        roiEditorView->setCameraConnected(stage);
        if (!liveRoom) { roiEditorView->setLive(false); roiEditorView->setZones({}); }
    }
    if (imgWipe_ && !liveRoom) imgWipe_->clearFrames();

    // 인스펙터는 101호와 같은 화면을 그대로 보여주되 만질 수 없게 한다.
    // 값까지 남겨두면 102호 화면에 101호 IP가 회색으로 읽혀 더 헷갈리므로 비운다
    // (돌아올 때 camFormBackup_에서 되돌린다).
    if (camControlStack) camControlStack->setEnabled(liveRoom);
    if (!liveRoom) {
        if (camIpEdit)   camIpEdit->clear();
        if (camUserEdit) camUserEdit->clear();
        if (camPwEdit)   camPwEdit->clear();
        if (discoveryTable) { discoveryTable->setRowCount(0); syncDiscoveryTableHeight(); }
        if (discoveryStatus)
            discoveryStatus->setText(QStringLiteral("이 방에는 아직 카메라가 배정되지 않았습니다."));
    }
    rebuildBedList();
    for (int r = 0; r < camRoomBtns_.size(); ++r)
        camRoomBtns_[r]->setChecked(r == selectedRoom_);
    setCamMode(camMode_);          // 빈 방이면 안내 페이지로, 아니면 원래 모드로
    refreshCamChannelStatus();

    rebuildVitalCards();     // 빈 방에서는 카드가 전부 "대기"로 내려간다
    refreshResourceTree();
}

// 타일 좌상단 이름. 빈 방에서는 입소자 이름을 붙이지 않는다 — 102호 타일에
// 101호 입소자 이름이 뜨면 그 사람이 거기 있는 것으로 읽힌다.
QString MainWindow::tileDisplayName(int ch) const
{
    if (selectedRoom_ != 0) return QStringLiteral("CH%1").arg(ch + 1);
    return channelDisplayName(ch);
}

void MainWindow::refreshResourceTree()
{
    if (!resourceTree_) return;

    const QString filter = resourceSearch_ ? resourceSearch_->text().trimmed() : QString();
    const QColor liveColor(QString::fromLatin1(kNormal));
    const QColor offColor(QString::fromLatin1(kTextSub));
    const QColor textColor(QString::fromLatin1(kTextMain));
    const QColor selColor(QString::fromLatin1(kSelect));

    // 방 그룹 갱신. 0번 방만 실제 카메라를 갖고, 나머지는 빈 자리라 회색으로 낮춘다.
    const QStringList rooms = roomNames();
    if (camItems_[0] && camItems_[0]->parent()) {
        QTreeWidgetItem* live = camItems_[0]->parent();
        live->setText(0, rooms.value(0, currentRoomName()));
        // 색을 명시해 둔다 — 기본색에 맡기면 아래 빈 방(회색)과 톤이 비슷해져
        // "카메라가 붙은 방"과 "빈 자리"가 구분되지 않는다.
        live->setForeground(0, selectedRoom_ == 0 ? selColor : textColor);
        QFont lf = live->font(0);
        lf.setBold(selectedRoom_ == 0);
        live->setFont(0, lf);
    }

    if (auto* root = resourceTree_->topLevelItem(0)) {
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* group = root->child(i);
            if (group->data(0, Qt::UserRole).toString() != QLatin1String("groupEmpty"))
                continue;
            const int r = group->data(0, Qt::UserRole + 1).toInt();
            const bool viewing = (r == selectedRoom_);
            group->setForeground(0, viewing ? selColor : offColor);
            QFont gf = group->font(0);
            gf.setBold(viewing);
            group->setFont(0, gf);
            for (int c = 0; c < group->childCount(); ++c) {
                QTreeWidgetItem* slot = group->child(c);
                slot->setForeground(0, offColor);
                slot->setIcon(0, QIcon(navIconPixmap(5, offColor, 16)));
            }
            // 검색 중이면 이름이 걸리는 빈 방만 남긴다(트리와 같은 규칙).
            group->setHidden(!filter.isEmpty() &&
                             !group->text(0).contains(filter, Qt::CaseInsensitive));
        }
    }

    for (int ch = 0; ch < 4; ++ch) {
        auto* it = camItems_[ch];
        if (!it) continue;

        const bool live = channelViews[ch] && channelViews[ch]->live();
        it->setText(0, channelDisplayName(ch));
        it->setIcon(0, QIcon(navIconPixmap(5, live ? liveColor : offColor, 16)));
        it->setForeground(0, tileHidden_[ch] ? offColor
                                             : (ch == selectedChannel_ ? selColor : textColor));
        QFont f = it->font(0);
        f.setBold(ch == selectedChannel_);
        f.setStrikeOut(tileHidden_[ch]);   // 레이아웃에서 물러난 타일
        it->setFont(0, f);
        it->setToolTip(0, live ? QStringLiteral("실시간 수신 중")
                               : QStringLiteral("영상 신호 없음"));

        // 검색어가 있으면 이름에 안 들어있는 카메라는 감춘다.
        it->setHidden(!filter.isEmpty() &&
                      !channelDisplayName(ch).contains(filter, Qt::CaseInsensitive));

        // 접힘 상태의 칩도 같은 값으로 갱신한다(트리와 어긋나면 안 된다).
        if (auto* chip = resChipBtns_[ch]) {
            chip->setChecked(ch == selectedChannel_);
            chip->setProperty("state", tileHidden_[ch] ? QStringLiteral("hidden")
                                       : live         ? QStringLiteral("live")
                                                      : QStringLiteral("off"));
            chip->setToolTip(QStringLiteral("%1 · %2")
                                 .arg(channelDisplayName(ch),
                                      live ? QStringLiteral("수신 중")
                                           : QStringLiteral("신호 없음")));
            chip->style()->unpolish(chip);
            chip->style()->polish(chip);
        }
    }
}

void MainWindow::setResourceCollapsed(bool on)
{
    resourceCollapsed_ = on;
    if (resourceBody_) resourceBody_->setVisible(!on);
    if (resourceRail_) resourceRail_->setVisible(on);
    // 제목까지 남겨 두면 "리소스" 세 글자가 폭을 못 줄여 토글 버튼이 밖으로
    // 밀려 나간다 — 접힌 패널에 화살표조차 없어 다시 펼 수가 없었다.
    if (resourceHead_) resourceHead_->setVisible(!on);

    if (resourcePanel_) {
        resourcePanel_->setFixedWidth(on ? 48 : 208);
        if (auto* l = qobject_cast<QVBoxLayout*>(resourcePanel_->layout()))
            l->setContentsMargins(on ? 9 : 10, 12, on ? 9 : 10, 12);
    }
    if (resourceToggle_) {
        // 화살표는 "누르면 갈 방향"을 가리킨다: 접힘→오른쪽으로 펼침(›),
        // 펼침→왼쪽으로 접힘(‹).
        resourceToggle_->setText(on ? QStringLiteral("›") : QStringLiteral("‹"));
        resourceToggle_->setToolTip(on ? QStringLiteral("리소스 목록 펼치기")
                                       : QStringLiteral("리소스 목록 접기"));
    }
    // 접히면 토글만 남으므로 가운데로, 펼치면 제목 오른쪽 끝으로.
    if (resourceToggle_ && resourceToggle_->parentWidget()) {
        if (auto* lay = resourceToggle_->parentWidget()->layout())
            lay->setAlignment(resourceToggle_, on ? Qt::AlignHCenter : Qt::AlignRight);
    }

    QSettings st;
    st.setValue(QStringLiteral("ui/resource_collapsed_v2"), on);
    refreshResourceTree();   // 칩 상태도 같은 경로에서 갱신
}

// 타일 오버레이·트리가 공유하는 채널 이름. "CH1 · 김복순" 형식.
QString MainWindow::channelDisplayName(int ch) const
{
    const QString tag = QStringLiteral("CH%1").arg(ch + 1);
    if (ch < 0 || ch >= 4) return tag;

    const QVector<int>& ids = residentsByChannel_[ch];
    if (ids.isEmpty()) return tag;

    // 한 채널(=한 방의 한 시야)에 여러 명이 함께 누워 있는 게 이 시스템의 기본
    // 구성이다. 그런데 대표 한 명만 쓰고 "외 N명"으로 접으면, 정작 그 방에 누가
    // 있는지를 화면에서 알 수 없다 — 두 명까지는 다 적는다.
    // 세 명 이상은 한 줄짜리 이름표가 감당이 안 되므로 그때만 접는다.
    QStringList names;
    for (int rid : ids) {
        const QString n = residentInfo_.value(rid).name;
        if (!n.isEmpty()) names << n;
    }
    if (names.isEmpty()) return tag;

    if (names.size() <= 2)
        return QStringLiteral("%1 · %2").arg(tag, names.join(QStringLiteral(", ")));
    return QStringLiteral("%1 · %2 외 %3명")
        .arg(tag, names.mid(0, 2).join(QStringLiteral(", ")))
        .arg(names.size() - 2);
}

// ═════════════════════════════════════════════════════════
//  하단 트랜스포트 + 타임라인 — Wisenet Viewer 하단 바에 대응.
//  라이브/재생 전환은 영상 영역을 통째로 갈아끼운다(liveOrPlaybackStack_).
// ═════════════════════════════════════════════════════════
QWidget* MainWindow::buildTransportBar()
{
    auto* wrap = new QWidget();
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // ── 컴트롤 줄 ──
    auto* bar = new QFrame();
    bar->setObjectName("transportBar");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(6);

    // 라이브 / 재생 토글 — Wisenet은 오른쪽 끝에 둔다.
    liveModeBtn_ = new QPushButton(QStringLiteral("라이브"));
    liveModeBtn_->setObjectName("modeBtn");
    liveModeBtn_->setCheckable(true);
    liveModeBtn_->setChecked(true);
    liveModeBtn_->setAutoExclusive(true);
    liveModeBtn_->setCursor(Qt::PointingHandCursor);
    connect(liveModeBtn_, &QPushButton::clicked, this, [this] { setPlaybackMode(false); });

    playbackModeBtn_ = new QPushButton(QStringLiteral("녹화 재생"));
    playbackModeBtn_->setObjectName("modeBtn");
    playbackModeBtn_->setCheckable(true);
    playbackModeBtn_->setAutoExclusive(true);
    playbackModeBtn_->setCursor(Qt::PointingHandCursor);
    connect(playbackModeBtn_, &QPushButton::clicked, this, [this] { setPlaybackMode(true); });

    // 트랜스포트 — 재생 모드에서만 의미가 있다.
    struct TSpec { const char16_t* glyph; const char16_t* tip; int deltaMs; };
    const TSpec specs[5] = {
        {u"⏮", u"10분 이전으로", -600000},
        {u"⏪", u"10초 이전으로", -10000},
        {u"▶", u"재생 / 일시정지", 0},
        {u"⏩", u"10초 이후로", 10000},
        {u"⏭", u"10분 이후로", 600000},
    };
    for (int i = 0; i < 5; ++i) {
        auto* b = new QPushButton(QString::fromUtf16(specs[i].glyph));
        b->setObjectName("transportBtn");
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 26);
        if (i == 2) {
            transportPlayBtn_ = b;
            b->setToolTip(QStringLiteral("재생 / 일시정지"));
            connect(b, &QPushButton::clicked, this, [this] {
                if (!playbackMode_ || !playbackPlayer_) return;
                if (playbackPlayer_->playbackState() == QMediaPlayer::PlayingState)
                    playbackPlayer_->pause();
                else
                    playbackPlayer_->play();
            });
        } else {
            b->setToolTip(QString::fromUtf16(specs[i].tip));
            connect(b, &QPushButton::clicked, this, [this, d = specs[i].deltaMs] {
                if (!playbackMode_ || !timeline_) return;
                seekPlaybackTo(timeline_->playhead() + d);
            });
        }
        h->addWidget(b);
    }

    transportTimeLabel_ = new QLabel();
    transportTimeLabel_->setObjectName("transportTime");
    h->addWidget(transportTimeLabel_);

    h->addStretch();

    transportSpeedCombo_ = new QComboBox();
    transportSpeedCombo_->setObjectName("transportSpeed");
    const double rates[5] = {0.5, 1.0, 2.0, 4.0, 8.0};
    for (double r : rates)
        transportSpeedCombo_->addItem(QStringLiteral("x%1").arg(r), r);
    transportSpeedCombo_->setCurrentIndex(1);
    connect(transportSpeedCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (playbackPlayer_ && transportSpeedCombo_)
                    playbackPlayer_->setPlaybackRate(
                        transportSpeedCombo_->currentData().toDouble());
            });
    h->addWidget(transportSpeedCombo_);

    h->addWidget(liveModeBtn_);
    h->addWidget(playbackModeBtn_);
    v->addWidget(bar);

    // ── 타임라인 ──
    timeline_ = new TimelineBar();
    timeline_->setObjectName("timeline");
    connect(timeline_, &TimelineBar::seekRequested, this, [this](qint64 ms) {
        if (transportTimeLabel_)
            transportTimeLabel_->setText(
                QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss"));
    });
    // 실제 탐색은 드래그가 끝난 뒤에만 한다 — 끌고 가는 동안 매 프레임
    // 새 세그먼트를 여는 건 네트워크와 디코더에 과부하다.
    connect(timeline_, &TimelineBar::seekCommitted, this, &MainWindow::seekPlaybackTo);
    v->addWidget(timeline_);

    // 라이브일 때 타임라인 창을 "지금"에 맞춰 계속 민다.
    connect(&timelineTimer_, &QTimer::timeout, this, [this] {
        if (!timeline_) return;
        if (!playbackMode_) {
            timeline_->followNow();
            if (transportTimeLabel_)
                transportTimeLabel_->setText(
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        }
    });
    timelineTimer_.start(1000);

    setPlaybackMode(false);
    // 세그먼트 목록을 한 번 받아 둔다. 예전엔 이벤트 기록 페이지의 NVR 카드가
    // 이 일을 겸했는데, 그 카드를 없앴으니 타임라인이 직접 챙긴다.
    refreshNvrSegments();
    return wrap;
}

// NVR 세그먼트 목록과 이벤트 이력을 타임라인이 이해하는 모양으로 넘긴다.
// 선택 채널만 보여준다 — 4개 채널을 한 줄에 곹쳐 그리면 어느 게 어느 채널인지 모른다.
void MainWindow::refreshTimeline()
{
    if (!timeline_) return;

    // 세그먼트 1개 길이는 서버 설정(nvr_segment_minutes) 기본값과 같다.
    // 목록은 시작 시각만 주므로 끝은 이 길이로 추정하고, 연속된 것끼리 합친다.
    constexpr qint64 kSegLenMs = 10 * 60 * 1000;
    constexpr qint64 kJoinGapMs = 30 * 1000;   // 이보다 밀착하면 한 구간으로 본다

    QVector<qint64> starts;
    for (const auto& seg : nvrSegments_)
        if (seg.channel == selectedChannel_) starts.push_back(seg.startMs);
    std::sort(starts.begin(), starts.end());

    QVector<TimelineBar::Span> spans;
    for (qint64 st : starts) {
        if (!spans.isEmpty() && st - spans.back().endMs <= kJoinGapMs)
            spans.back().endMs = st + kSegLenMs;
        else
            spans.push_back({st, st + kSegLenMs});
    }
    timeline_->setSpans(spans);

    QVector<TimelineBar::Marker> markers;
    for (const auto& e : timelineEvents_)
        if (e.channel == selectedChannel_) markers.push_back({e.atMs, e.color});
    timeline_->setMarkers(markers);
}

// 라이브 ↔ 녹화 재생 전환. 영상 영역을 통째로 바꿔 두 모드가 섞이지 않게 한다.
void MainWindow::setPlaybackMode(bool on)
{
    playbackMode_ = on;
    if (liveModeBtn_) liveModeBtn_->setChecked(!on);
    if (playbackModeBtn_) playbackModeBtn_->setChecked(on);
    if (timeline_) timeline_->setLiveMode(!on);
    if (liveOrPlaybackStack_) {
        // 재생 모드로 막 넘어왔을 땐 아직 볼 시각을 고르지 않았다 → 안내 화면(2).
        liveOrPlaybackStack_->setCurrentIndex(on ? 2 : 0);
    }
    if (on && playbackPlaceholder_) {
        playbackPlaceholder_->setText(QStringLiteral(
            "아래 타임라인에서 볼 시각을 클릭하세요.\n"
            "초록 구간이 %1 녹화가 남아 있는 시간대입니다.")
                .arg(channelDisplayName(selectedChannel_)));
    }

    if (!on) {
        if (playbackPlayer_) playbackPlayer_->stop();
        if (timeline_) timeline_->followNow();
        return;
    }

    // 재생 모드로 들어오면 최신 녹화 목록을 한 번 받아온다.
    refreshNvrSegments();
}

// 그 시각을 담은 NVR 세그먼트를 찾아 그 안의 상대 위치로 재생한다.
void MainWindow::seekPlaybackTo(qint64 ms)
{
    if (!timeline_) return;
    timeline_->setPlayhead(ms);
    if (transportTimeLabel_)
        transportTimeLabel_->setText(
            QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss"));

    if (!playbackMode_) {
        // 라이브 중에 타임라인을 찍으면 재생 모드로 넘어가는 게 자연스럽다.
        setPlaybackMode(true);
    }

    constexpr qint64 kSegLenMs = 10 * 60 * 1000;
    const NvrSegmentInfo* best = nullptr;
    for (const auto& seg : nvrSegments_) {
        if (seg.channel != selectedChannel_) continue;
        if (ms < seg.startMs || ms >= seg.startMs + kSegLenMs) continue;
        if (!best || seg.startMs > best->startMs) best = &seg;
    }
    if (!best) {
        if (transportTimeLabel_)
            transportTimeLabel_->setText(
                QStringLiteral("%1 · 녹화 없음")
                    .arg(QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm:ss")));
        if (playbackPlayer_) playbackPlayer_->stop();
        if (playbackPlaceholder_)
            playbackPlaceholder_->setText(
                QStringLiteral("%1 에는 %2 녹화가 없습니다.\n"
                               "타임라인의 초록 구간을 클릭하세요.")
                    .arg(QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm:ss"),
                         channelDisplayName(selectedChannel_)));
        if (liveOrPlaybackStack_) liveOrPlaybackStack_->setCurrentIndex(2);
        return;
    }
    if (liveOrPlaybackStack_) liveOrPlaybackStack_->setCurrentIndex(1);

    if (!playbackPlayer_) {
        playbackPlayer_ = new QMediaPlayer(this);
        playbackPlayer_->setVideoOutput(playbackVideo_);
        // 재생이 흘러가면 플레이헤드도 같이 움직여야 타임라인이 거짓말을 안 한다.
        connect(playbackPlayer_, &QMediaPlayer::positionChanged, this,
                [this](qint64 pos) {
                    if (!playbackMode_ || !timeline_) return;
                    const qint64 at = playbackSegStartMs_ + pos;
                    timeline_->setPlayhead(at);
                    if (transportTimeLabel_)
                        transportTimeLabel_->setText(
                            QDateTime::fromMSecsSinceEpoch(at)
                                .toString("yyyy-MM-dd HH:mm:ss"));
                });
        // ▶ 버튼 글리프를 실제 재생 상태에 맞춰 토글(블랙박스 재생기의
        // playbackStateChanged 연결과 같은 패턴 — 예전엔 이 트랜스포트 바에만 빠져있었음).
        connect(playbackPlayer_, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
                    if (transportPlayBtn_)
                        transportPlayBtn_->setText(st == QMediaPlayer::PlayingState
                                                        ? QStringLiteral("⏸")
                                                        : QStringLiteral("▶"));
                });
    }

    const QString url = best->url;
    const qint64 offset = ms - best->startMs;
    playbackSegStartMs_ = best->startMs;

    if (playbackPlayer_->source() != QUrl(url)) {
        playbackPlayer_->setSource(QUrl(url));
        // 길이를 알기 전에 setPosition을 불러도 무시된다 — 한 번만 적용하고 끊는다.
        auto* conn = new QMetaObject::Connection();
        *conn = connect(playbackPlayer_, &QMediaPlayer::durationChanged, this,
                        [this, offset, conn](qint64 dur) {
                            if (dur <= 0) return;
                            playbackPlayer_->setPosition(qBound(qint64(0), offset, dur));
                            disconnect(*conn);
                            delete conn;
                        });
    } else {
        playbackPlayer_->setPosition(offset);
    }
    playbackPlayer_->setPlaybackRate(
        transportSpeedCombo_ ? transportSpeedCombo_->currentData().toDouble() : 1.0);
    playbackPlayer_->play();
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
    const QStringList rooms = roomNames();
    const QString room = rooms.value(selectedRoom_, currentRoomName());
    // 빈 방에는 배정된 사람이 없다 — 여기서 residentsByChannel_를 그대로 쓰면
    // 102호 화면에 101호 입소자의 심박이 뜬다.
    static const QVector<int> kNoResidents;
    for (int ch = 0; ch < 4; ++ch) {
        const QString bedText = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
        const QVector<int>& ids =
            (selectedRoom_ == 0) ? residentsByChannel_[ch] : kNoResidents;

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
//  TAB2: 이벤트 기록 — 상단 요약 카드 + 필터 + [로그 표 | 인라인 블랙박스+NVR].
//  왼쪽 표의 행을 열면 페이지 이동 없이 바로 이 페이지 오른쪽에서 재생된다
//  (onLogRowActivated). 영상 검색 페이지의 AI 검색 결과 클립도 같은 재생기로
//  오게 하려면 이 페이지로 이동시키는 쪽을 택했다(재생기를 두 곳에 중복해서
//  두지 않기 위해) — buildVideoSearchTab() 참고.
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

    // 본문 3단: 좌측 필터 컬럼 / 가운데 로그 표 / 우측 재생기 + NVR 탐색.
    // Wisenet Viewer의 Event search 창과 같은 순서다 — 조건→결과→미리보기.
    auto* body = new QHBoxLayout();
    body->setSpacing(12);
    body->addWidget(buildSearchFilters(), 0);
    body->addWidget(buildLogTable(), 5);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);
    auto* bbCap = new QLabel(QStringLiteral("블랙박스"));
    bbCap->setObjectName("panelTitle");
    right->addWidget(bbCap);
    auto* bbHint = new QLabel(
        QStringLiteral("왼쪽 표에서 이벤트를 고르면 여기서 바로 재생됩니다."));
    bbHint->setObjectName("subtitle");
    bbHint->setWordWrap(true);
    right->addWidget(bbHint);

    // 지금 보고 있는 게 어느 사건인지 — 표에서 눈을 떼면 잊는다.
    eventContextLabel = new QLabel();
    eventContextLabel->setObjectName("eventContext");
    eventContextLabel->setWordWrap(true);
    eventContextLabel->hide();   // 행을 고르기 전엔 보여줄 게 없다
    right->addWidget(eventContextLabel);
    right->addWidget(buildBlackboxPlayer(), 1);

    auto* actionRow = new QHBoxLayout();
    nvrJumpButton = new QPushButton(QStringLiteral("이 시점 NVR에서 이어보기"));
    nvrJumpButton->setObjectName("nvrJumpButton");
    nvrJumpButton->setCursor(Qt::PointingHandCursor);
    nvrJumpButton->setEnabled(false);   // 행을 열기 전까진 대상이 없음
    connect(nvrJumpButton, &QPushButton::clicked, this, &MainWindow::jumpToNvrContext);
    actionRow->addWidget(nvrJumpButton, 1);

    clipDownloadButton = new QPushButton(QStringLiteral("다운로드"));
    clipDownloadButton->setObjectName("clipDownloadButton");
    clipDownloadButton->setCursor(Qt::PointingHandCursor);
    clipDownloadButton->setEnabled(false);   // 재생된 클립이 있어야 저장할 게 있음
    connect(clipDownloadButton, &QPushButton::clicked, this, &MainWindow::downloadCurrentClip);
    actionRow->addWidget(clipDownloadButton);
    right->addLayout(actionRow);

    // NVR 세그먼트 목록 카드는 없앴다 — 같은 nvrSegments_를 관제화면 하단
    // 타임라인이 시간축 위에 그려 주고 클릭 탐색까지 된다. 파일 목록은 그 데이터를
    // "채널 1 · 11:00:00" 같은 파일명으로 늘어놓을 뿐이라, 원하는 시각을 찾으려면
    // 파일명에서 시간을 역산해야 했다. 이 페이지에 남는 NVR 접점은
    // [이 시점 NVR에서 이어보기] 하나 — 이벤트에서 그 순간으로 건너뛰는 링크다.
    body->addLayout(right, 4);

    outer->addLayout(body, 1);

    // 페이지를 만들자마자 원장을 한 번 읽는다 — 예전엔 이 표가 세션 메모리만
    // 담고 있어서 앱을 켤 때마다 비어 있었다.
    reloadEventLog();
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
// QCalendarWidget의 요일 머리글(일~토)은 QSS의 QHeaderView::section 규칙이 닿지
// 않는다 — 헤더는 위젯 내부에서 QTextCharFormat으로 그려지기 때문이다. 그래서
// 다크 테마에서도 흰 바탕 한 줄이 남아 있었다. 배경/글자색을 코드로 직접 준다.
// (주말 색은 QCalendarWidget 기본값인 빨강을 유지하되 팔레트 톤에 맞춘다.)
void MainWindow::applyCalendarPalette(QCalendarWidget* cal)
{
    if (!cal) return;

    QTextCharFormat head;
    head.setBackground(QColor(QString::fromLatin1(kCard)));
    head.setForeground(QColor(QString::fromLatin1(kTextSub)));
    head.setFontWeight(QFont::Bold);
    cal->setHeaderTextFormat(head);

    // 평일/주말 본문 색도 같은 경로로 맞춘다 — QSS로는 요일별 색을 못 준다.
    QTextCharFormat weekday;
    weekday.setForeground(QColor(QString::fromLatin1(kTextMain)));
    for (int d = Qt::Monday; d <= Qt::Friday; ++d)
        cal->setWeekdayTextFormat(Qt::DayOfWeek(d), weekday);

    QTextCharFormat weekend;
    weekend.setForeground(QColor(QString::fromLatin1(kCritical)));
    cal->setWeekdayTextFormat(Qt::Saturday, weekend);
    cal->setWeekdayTextFormat(Qt::Sunday, weekend);
}

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
    applyCalendarPalette(reportCalendar);
    lay->addWidget(reportCalendar);

    // 오늘로 되돌아오는 버튼 — 과거를 뒤지다 보면 오늘 찾아 돌아오기가 번거롭다.
    auto* today = new QPushButton(QStringLiteral("오늘"));
    today->setObjectName("reportTodayBtn");
    today->setCursor(Qt::PointingHandCursor);
    connect(today, &QPushButton::clicked, this, [this] {
        reportCalendar->setSelectedDate(QDate::currentDate());
    });
    lay->addWidget(today);

    // TODO(리포트): AI 요약 버튼이 이 아래에 들어간다.
    auto* pdfBtn = new QPushButton(QStringLiteral("PDF 내보내기"));
    pdfBtn->setObjectName("reportTodayBtn");
    pdfBtn->setCursor(Qt::PointingHandCursor);
    connect(pdfBtn, &QPushButton::clicked, this, &MainWindow::exportReportPdf);
    lay->addWidget(pdfBtn);

    summaryBtn = new QPushButton(QStringLiteral("요약"));
    summaryBtn->setObjectName("reportTodayBtn");
    summaryBtn->setCursor(Qt::PointingHandCursor);
    connect(summaryBtn, &QPushButton::clicked, this, &MainWindow::requestAiSummary);
    lay->addWidget(summaryBtn);

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

    // ── AI 요약 ──
    // 위쪽 숫자는 기계가 센 값이고 이 문단은 생성된 글이다. 감사 상황에서 그
    // 구분이 중요하므로 배경을 달리해 시각적으로 떼어 놓는다.
    auto* sumCap = new QLabel(QStringLiteral("요약"));
    sumCap->setObjectName("careBigCap");
    lay->addWidget(sumCap);

    summaryLabel = new QLabel();
    summaryLabel->setObjectName("aiSummary");
    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(summaryLabel);

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
            if (q.exec() && q.next()) {
                // 호실·침대는 아직 안 채운 입소자가 많다. 빈 값을 그대로 이으면
                // "호 ·  · 위험도 하" 처럼 구분점만 남아 오히려 지저분하다.
                QStringList parts;
                const QString room = q.value(0).toString().trimmed();
                const QString bed  = q.value(1).toString().trimmed();
                if (!room.isEmpty()) parts << QStringLiteral("%1호").arg(room);
                if (!bed.isEmpty())  parts << bed;
                parts << QStringLiteral("위험도 %1").arg(q.value(2).toString());
                meta = parts.join(QStringLiteral(" · "));
            }
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
        const QString when = QStringLiteral("%1 (%2)")
                                 .arg(date.toString(QStringLiteral("yyyy-MM-dd")),
                                      koreanDow(date));
        reportDateLabel->setText(date == QDate::currentDate()
                                     ? QStringLiteral("%1 · 오늘").arg(when)
                                     : when);
    }
    updateCareTime();
}

QWidget* MainWindow::buildSearchFilters()
{
    // Wisenet Viewer의 Event search 좌측 패널과 같은 배치 — 조건을 세로로 쌓고
    // 결과 표는 오른쪽에 넓게 둔다. 조건이 바뀌면 화면의 행을 숨기는 게 아니라
    // events 원장을 다시 조회한다(reloadEventLog) — 그래야 지난 기록이 나온다.
    auto* bar = new QFrame();
    bar->setObjectName("filterBar");
    bar->setFixedWidth(206);

    auto* lay = new QVBoxLayout(bar);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(6);

    auto* cap = new QLabel(QStringLiteral("검색 조건"));
    cap->setObjectName("filterCap");
    lay->addWidget(cap);
    lay->addSpacing(4);

    // 조건을 바꾸면 즉시 다시 조회한다 — 별도 '검색' 버튼은 두지 않는다.
    filterDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    filterDateFrom->setCalendarPopup(true);
    filterDateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    filterDateTo = new QDateEdit(QDate::currentDate());
    filterDateTo->setCalendarPopup(true);
    filterDateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    connect(filterDateFrom, &QDateEdit::dateChanged, this,
            [this](const QDate&) { reloadEventLog(); });
    connect(filterDateTo, &QDateEdit::dateChanged, this,
            [this](const QDate&) { reloadEventLog(); });

    filterRoom = new QComboBox();
    // 카메라(4채널) 한 대 = 방 하나. 지금은 room이 하나뿐이라 목록도 하나지만,
    // 카메라가 늘어나면 currentRoomName() 자리가 room 목록으로 바뀌면서 여기도
    // 자연히 늘어나는 구조 — 로그 위치 문구("{room} · 채널 N")와 접두어를 맞춰야
    // applyLogFilters()의 startsWith 매칭이 먹는다.
    filterRoom->addItem(QStringLiteral("전체 병실"));
    filterRoom->addItem(currentRoomName());
    connect(filterRoom, &QComboBox::currentTextChanged,
            this, [this](const QString&) { applyLogFilters(true); });

    filterChannel = new QComboBox();
    filterChannel->addItem(QStringLiteral("전체 채널"), -1);
    for (int ch = 0; ch < 4; ++ch)
        filterChannel->addItem(QStringLiteral("채널 %1").arg(ch + 1), ch);
    connect(filterChannel, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { reloadEventLog(); });

    filterEventType = new QComboBox();
    filterEventType->addItems({QStringLiteral("전체 이벤트"), QStringLiteral("낙상"),
                               QStringLiteral("침상이탈"), QStringLiteral("생체신호 이상")});
    connect(filterEventType, &QComboBox::currentTextChanged,
            this, [this](const QString&) { reloadEventLog(); });

    filterConfirmed = new QComboBox();
    filterConfirmed->addItems({QStringLiteral("전체"), QStringLiteral("미확인만"),
                               QStringLiteral("확인만")});
    connect(filterConfirmed, &QComboBox::currentTextChanged,
            this, [this](const QString&) { reloadEventLog(); });

    // 기간 프리셋 — 관제사가 실제로 묻는 단위는 "오늘", "이번 주", "이번 달"이다.
    // 날짜 두 칸을 매번 돌리게 하면 그 질문을 하기까지 손이 너무 많이 간다.
    auto* presetRow = new QHBoxLayout();
    presetRow->setContentsMargins(0, 0, 0, 0);
    presetRow->setSpacing(4);
    const QPair<QString, int> presets[3] = {
        {QStringLiteral("오늘"), 0}, {QStringLiteral("7일"), 6}, {QStringLiteral("30일"), 29}};
    for (const auto& pr : presets) {
        auto* b2 = new QPushButton(pr.first);
        b2->setObjectName("filterResetBtn");
        b2->setCursor(Qt::PointingHandCursor);
        connect(b2, &QPushButton::clicked, this, [this, days = pr.second] {
            // 두 날짜를 연달아 바꾸면 dateChanged가 두 번 떠 조회가 두 번 돈다.
            // 신호를 잠깐 막고 마지막에 한 번만 조회한다.
            const QSignalBlocker b1(filterDateFrom), b2(filterDateTo);
            filterDateFrom->setDate(QDate::currentDate().addDays(-days));
            filterDateTo->setDate(QDate::currentDate());
            reloadEventLog();
        });
        presetRow->addWidget(b2);
    }
    lay->addLayout(presetRow);
    lay->addSpacing(6);

    // 라벨을 필드 위에 얹는다 — 좁은 컬럼에서 라벨과 값을 한 줄에 두면 값이 눌린다.
    auto addField = [&](const QString& label, QWidget* w) {
        auto* l = new QLabel(label);
        l->setObjectName("filterFieldCap");
        lay->addWidget(l);
        lay->addWidget(w);
        lay->addSpacing(6);
    };
    addField(QStringLiteral("시작일"), filterDateFrom);
    addField(QStringLiteral("종료일"), filterDateTo);
    addField(QStringLiteral("병실"), filterRoom);
    addField(QStringLiteral("채널"), filterChannel);
    addField(QStringLiteral("이벤트 종류"), filterEventType);
    addField(QStringLiteral("확인 여부"), filterConfirmed);

    auto* resetBtn = new QPushButton(QStringLiteral("조건 초기화"));
    resetBtn->setObjectName("filterResetBtn");
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        const QSignalBlocker b1(filterDateFrom), b2(filterDateTo), b3(filterRoom),
                             b4(filterChannel), b5(filterEventType), b6(filterConfirmed);
        filterDateFrom->setDate(QDate::currentDate().addDays(-7));
        filterDateTo->setDate(QDate::currentDate());
        filterRoom->setCurrentIndex(0);
        filterChannel->setCurrentIndex(0);
        filterEventType->setCurrentIndex(0);
        filterConfirmed->setCurrentIndex(0);
        reloadEventLog();
    });
    lay->addWidget(resetBtn);

    lay->addStretch();
    return bar;
}

QWidget* MainWindow::buildLogTable()
{
    auto* wrap = new QWidget();
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    // 결과 건수 — Wisenet Event search 하단의 "N Results"에 해당한다.
    // 미확인 수를 같이 보여준다: 관제사가 실제로 처리해야 할 양이 그 숫자다.
    auto* head = new QHBoxLayout();
    head->setContentsMargins(2, 0, 2, 0);
    logCountLabel = new QLabel();
    logCountLabel->setObjectName("logCount");
    head->addWidget(logCountLabel);
    head->addStretch();

    auto* reloadBtn = new QPushButton(QStringLiteral("새로고침"));
    reloadBtn->setObjectName("filterResetBtn");
    reloadBtn->setCursor(Qt::PointingHandCursor);
    reloadBtn->setToolTip(QStringLiteral("서버 원장에서 다시 조회"));
    connect(reloadBtn, &QPushButton::clicked, this, &MainWindow::reloadEventLog);
    head->addWidget(reloadBtn);
    v->addLayout(head);

    logTable = new QTableWidget(0, LogColCount);
    logTable->setObjectName("logTable");
    logTable->setHorizontalHeaderLabels(
        {QStringLiteral("발생시각"), QStringLiteral("종류"),
         QStringLiteral("위치"), QStringLiteral("입소자"),
         QStringLiteral("출처"), QStringLiteral("상태")});
    auto* hh = logTable->horizontalHeader();
    // 발생시각은 "yyyy-MM-dd HH:mm:ss"가 잘리지 않게 내용 폭, 위치만 남는 폭을 먹는다.
    hh->setSectionResizeMode(LogWhen, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogType, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogPlace, QHeaderView::Stretch);
    hh->setSectionResizeMode(LogResident, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogSource, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogStatus, QHeaderView::ResizeToContents);
    hh->setHighlightSections(false);
    logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    logTable->setSelectionMode(QAbstractItemView::SingleSelection);
    logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable->setShowGrid(false);                       // 격자선 대신 행 여백으로 구분
    logTable->setAlternatingRowColors(true);            // 얼룩 배경으로 행 가독성 ↑
    logTable->verticalHeader()->setVisible(false);      // 행 번호 숨김
    logTable->verticalHeader()->setDefaultSectionSize(34);
    logTable->setCursor(Qt::PointingHandCursor);
    // 한 번 클릭 = 미리보기(Wisenet도 목록에서 고르면 바로 오른쪽에 뜬다).
    // 더블클릭까지 기다리게 하면 "왜 안 나오지"를 매번 겪는다.
    connect(logTable, &QTableWidget::cellClicked, this, &MainWindow::onLogRowActivated);

    // 조회 결과가 0건일 때 표 한가운데 띄우는 안내. 표 위에 겹쳐 놓고
    // 행이 없을 때만 보이게 한다 — 빈 격자만 남으면 고장으로 읽힌다.
    logEmptyHint = new QLabel(logTable);
    logEmptyHint->setObjectName("logEmptyHint");
    logEmptyHint->setAlignment(Qt::AlignCenter);
    logEmptyHint->setWordWrap(true);
    logEmptyHint->hide();

    v->addWidget(logTable, 1);
    return wrap;
}

// events 원장을 현재 필터 조건으로 조회해 표를 통째로 다시 채운다.
//
// 예전엔 이 표가 "이번 세션에 소켓으로 들어온 이벤트"만 담고 있어서, 앱을 껐다
// 켜면 항상 비어 있었다(서버는 그동안 계속 DB에 쌓고 있었다). 필터도 화면에 있는
// 행을 숨기는 방식이라 조회 범위가 곧 세션 길이였다. 이제 조건은 SQL로 내려간다.
void MainWindow::reloadEventLog()
{
    if (!logTable) return;

    if (!QSqlDatabase::database().isOpen()) {
        logTable->setRowCount(0);
        refreshLogSummary();
        return;
    }

    // 500행 상한 — 관제 PC에서 표가 버벅이지 않는 선. 넘치면 기간을 좁히라고 알린다.
    // (상한을 넘겼는지 알려면 한 줄 더 받아 봐야 한다 → LIMIT 501)
    static constexpr int kMaxRows = 500;

    QString sql = QStringLiteral(
        "SELECT e.event_id, UNIX_TIMESTAMP(e.occurred_at)*1000, e.camera_id, "
        "       e.event_type, e.source, e.clip_url, e.confirmed_at, e.confirmed_by, "
        "       r.name, r.room "
        "FROM events e LEFT JOIN residents r ON r.resident_id = e.resident_id "
        "WHERE DATE(e.occurred_at) BETWEEN ? AND ? ");

    QVariantList binds;
    binds << (filterDateFrom ? filterDateFrom->date() : QDate::currentDate().addDays(-7));
    binds << (filterDateTo ? filterDateTo->date() : QDate::currentDate());

    const QString typeSel = filterEventType ? filterEventType->currentText() : QString();
    if (typeSel == QStringLiteral("낙상"))            { sql += "AND e.event_type='FALL' "; }
    else if (typeSel == QStringLiteral("침상이탈")) { sql += "AND e.event_type='EGRESS' "; }
    else if (typeSel == QStringLiteral("생체신호 이상")) { sql += "AND e.event_type='VITAL_ABNORMAL' "; }

    if (filterChannel && filterChannel->currentData().toInt() >= 0) {
        sql += "AND e.camera_id = ? ";
        binds << filterChannel->currentData().toInt();
    }
    const QString confSel = filterConfirmed ? filterConfirmed->currentText() : QString();
    if (confSel == QStringLiteral("미확인만"))      sql += "AND e.confirmed_at IS NULL ";
    else if (confSel == QStringLiteral("확인만"))        sql += "AND e.confirmed_at IS NOT NULL ";

    sql += QStringLiteral("ORDER BY e.occurred_at DESC LIMIT %1").arg(kMaxRows + 1);

    QSqlQuery q;
    q.prepare(sql);
    for (const QVariant& b : binds) q.addBindValue(b);
    if (!q.exec()) {
        qDebug() << "이벤트 원장 조회 실패:" << q.lastError().text();
        return;
    }

    logTable->setSortingEnabled(false);
    logTable->setRowCount(0);

    int shown = 0;
    bool truncated = false;
    while (q.next()) {
        if (shown >= kMaxRows) { truncated = true; break; }

        EventLogRow e;
        e.eventId     = q.value(0).toLongLong();
        e.occurredMs  = q.value(1).toLongLong();
        e.channel     = q.value(2).isNull() ? -1 : q.value(2).toInt();
        e.typeCode    = q.value(3).toString();
        e.source      = q.value(4).toString();
        e.confirmed   = !q.value(6).isNull();
        e.confirmedBy = q.value(7).toString();
        e.residentName = q.value(8).toString();

        // clip_url 은 파일명만 저장된다(서버는 자기 외부 IP를 모른다 —
        // server/src/main.cpp 의 insertEvent 호출부 주석 참조). 호스트는 채널로 정해진다.
        const QString file = q.value(5).toString();
        if (!file.isEmpty() && e.channel >= 0) {
            e.clipUrl = file.startsWith(QLatin1String("http"))
                            ? file
                            : QStringLiteral("http://%1:%2/%3")
                                  .arg(hostForChannel(e.channel))
                                  .arg(kClipHttpPort)
                                  .arg(file);
        }

        // 위치 — 발생 당시 사람의 호실이 있으면 그걸 쓰고, 없으면 현재 방 이름.
        const QString room = q.value(9).toString();
        e.place = QStringLiteral("%1 · 채널 %2")
                      .arg(room.isEmpty() ? currentRoomName() : room)
                      .arg(e.channel + 1);

        insertEventRow(e);
        ++shown;
    }

    logTable->setSortingEnabled(true);
    logTable->sortItems(LogWhen, Qt::DescendingOrder);
    // 색·정렬은 refreshEventLog()가 입힌다(요약 갱신도 그 안에서 이어진다).
    // 이걸 건너뛰고 refreshLogSummary()만 부르면 방금 넣은 행들이 전경색 없이
    // 남아 표가 통째로 비어 보인다.
    refreshEventLog();

    if (truncated && logCountLabel) {
        logCountLabel->setText(
            logCountLabel->text() +
            QStringLiteral("  ·  %1행까지만 표시 — 기간을 좀 더 좁혀 보세요")
                .arg(kMaxRows));
    }
}

QString MainWindow::eventTypeLabel(const QString& code)
{
    if (code == QLatin1String("FALL"))   return QStringLiteral("낙상");
    if (code == QLatin1String("EGRESS")) return QStringLiteral("침상이탈");
    if (code == QLatin1String("VITAL_ABNORMAL")) return QStringLiteral("생체신호 이상");
    return code;
}

QString MainWindow::eventSourceLabel(const QString& code)
{
    if (code == QLatin1String("CAMERA"))   return QStringLiteral("카메라");
    if (code == QLatin1String("WEARABLE")) return QStringLiteral("웨어러블");
    return code;
}

// 표에 한 줄 넣기 — DB 조회분과 실시간 도착분이 공유하는 유일한 삽입 경로.
void MainWindow::insertEventRow(const EventLogRow& e)
{
    if (!logTable) return;

    const int row = logTable->rowCount();
    logTable->insertRow(row);

    auto* when = new QTableWidgetItem(
        QDateTime::fromMSecsSinceEpoch(e.occurredMs).toString("yyyy-MM-dd HH:mm:ss"));
    when->setData(LogClipUrl, e.clipUrl);
    when->setData(LogChannel, e.channel);
    when->setData(LogTimestamp, e.occurredMs);
    when->setData(LogEventId, e.eventId);
    logTable->setItem(row, LogWhen, when);

    logTable->setItem(row, LogType, new QTableWidgetItem(eventTypeLabel(e.typeCode)));
    logTable->setItem(row, LogPlace, new QTableWidgetItem(e.place));
    // 사람을 특정 못 한 이벤트를 빈칸으로 두면 "표가 덜 찼나"로 읽힌다 — 모른다고 쓴다.
    logTable->setItem(row, LogResident,
                      new QTableWidgetItem(e.residentName.isEmpty()
                                               ? QStringLiteral("신원 미상")
                                               : e.residentName));
    logTable->setItem(row, LogSource, new QTableWidgetItem(eventSourceLabel(e.source)));

    auto* st = new QTableWidgetItem(e.confirmed ? QStringLiteral("확인")
                                                : QStringLiteral("미확인"));
    if (e.confirmed && !e.confirmedBy.isEmpty())
        st->setToolTip(QStringLiteral("%1 확인").arg(e.confirmedBy));
    logTable->setItem(row, LogStatus, st);
}

// 실시간으로 도착한 이벤트를 표 맨 위에 얹는다.
// 서버도 같은 이벤트를 events 에 쓰므로, 다음 조회(reloadEventLog) 때 DB 쪽으로
// 자연히 합쳐진다 — 여기서 넣는 행은 event_id 가 없어 '확인'이 DB에 남지 않는다.
void MainWindow::appendLiveEvent(int channel, int roiId, qint64 occurredMs,
                                 const QString& typeCode, const QString& source)
{
    if (!logTable) return;

    EventLogRow e;
    e.occurredMs = occurredMs;
    e.channel = channel;
    e.typeCode = typeCode;
    e.source = source;
    e.place = eventPlaceLabel(channel, roiId);
    e.residentName = zoneResidentName(channel, roiId);
    // 서버 저장 규칙: ch{N}_{ms}_{TYPE}.mp4 (blackbox.trigger의 접미사와 같다)
    e.clipUrl = QStringLiteral("http://%1:%2/ch%3_%4_%5.mp4")
                    .arg(hostForChannel(channel))
                    .arg(kClipHttpPort)
                    .arg(channel)
                    .arg(occurredMs)
                    .arg(typeCode);

    const bool wasSorting = logTable->isSortingEnabled();
    logTable->setSortingEnabled(false);
    insertEventRow(e);
    logTable->setSortingEnabled(true);
    logTable->sortItems(LogWhen, Qt::DescendingOrder);   // 최신 이벤트가 위로
    applyLogFilters();
}

// 결과 건수/미확인 수 + 빈 상태 안내.
void MainWindow::refreshLogSummary()
{
    if (!logTable) return;

    int visible = 0, unconfirmed = 0;
    for (int r = 0; r < logTable->rowCount(); ++r) {
        if (logTable->isRowHidden(r)) continue;
        ++visible;
        auto* st = logTable->item(r, LogStatus);
        if (st && st->text() == QStringLiteral("미확인")) ++unconfirmed;
    }

    if (logCountLabel) {
        logCountLabel->setText(
            unconfirmed > 0
                ? QStringLiteral("%1건  ·  미확인 %2건").arg(visible).arg(unconfirmed)
                : QStringLiteral("%1건").arg(visible));
        logCountLabel->setProperty("alert", unconfirmed > 0);
        logCountLabel->style()->unpolish(logCountLabel);
        logCountLabel->style()->polish(logCountLabel);
    }

    if (logEmptyHint) {
        logEmptyHint->setVisible(visible == 0);
        if (visible == 0) {
            logEmptyHint->setText(
                QSqlDatabase::database().isOpen()
                    ? QStringLiteral("이 조건에 맞는 이벤트가 없습니다.\n"
                                     "왼쪽에서 기간을 넓히거나 종류·채널 필터를 풀어 보세요.")
                    : QStringLiteral("DB에 연결되지 않았습니다.\n"
                                     "연결되면 지난 이벤트 기록이 여기에 나타납니다."));
            logEmptyHint->setGeometry(logTable->viewport()->rect().adjusted(20, 40, -20, 0));
            logEmptyHint->raise();
        }
    }
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

    // "이 시점 NVR에서 이어보기" 대상(selectedEventChannel_/Timestamp_)을 URL의
    // 파일명에서 직접 뽑아 채운다. 예전엔 이벤트 기록 행(onLogRowActivated)만
    // 이 값을 채워서, 검색 결과 클립을 직접 눌렀을 땐 채워지지 않아 버튼이
    // 계속 비활성 상태였다.
    // ★ 블랙박스 이벤트 클립(ch{N}_{ms}_{TYPE}.mp4, 3토큰)에서만 채운다 — NVR
    //   세그먼트(ch{N}_{ms}.mp4, 2토큰)까지 여기 걸리면, "이 시점 NVR에서
    //   이어보기"로 방금 도착한 세그먼트를 볼 때 그 세그먼트 자신의 시작시각으로
    //   원래 이벤트 시각을 덮어써버려서, 다음 판정이 엉뚱한 세그먼트/오프셋을
    //   가리키게 되는 버그가 있었다. NVR 세그먼트 재생 중엔 이 값을 그대로 둬서
    //   "원래 보러 온 이벤트"를 계속 가리키게 한다.
    const QString fileName = QUrl(url).fileName();
    const QString stem = fileName.left(fileName.lastIndexOf('.'));
    const QStringList parts = stem.split(QLatin1Char('_'));
    if (parts.size() >= 3 && parts[0].startsWith(QLatin1String("ch"))) {
        bool chOk = false, msOk = false;
        const int ch = parts[0].mid(2).toInt(&chOk);
        const qint64 ms = parts[1].toLongLong(&msOk);
        if (chOk && msOk && ch >= 0 && ch < 4 && ms > 0) {
            selectedEventChannel_ = ch;
            selectedEventTimestampMs_ = ms;
        }
    }
    if (nvrJumpButton)
        nvrJumpButton->setEnabled(selectedEventChannel_ >= 0 && selectedEventTimestampMs_ >= 0);
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
            refreshTimeline();   // 세그먼트가 들어오면 하단 타임라인을 다시 그린다
        });
    }
}

// ═══════════════════════════════════════════════════════════
//  TAB: 영상 검색(🔍) — 좌측 네비의 독립 페이지. 케어봇과 같은 서버 로직
//  (video_search_module)을 관제 화면에서도 쓴다. 질의는 DBJ_CTRL_SEARCH_QUERY로
//  보내고, 응답(DBJ_SEARCH_MAGIC)은 onReadyRead가 받아 onSearchResultReceived로
//  넘긴다.
//  재생기는 이 페이지에 없다 — 이벤트 기록 페이지(kNavEventLog)의 인라인
//  재생기를 그대로 쓴다(재생기를 두 곳에 중복해서 두지 않기 위해). 검색
//  결과의 클립 링크를 누르면 그 페이지로 이동해 바로 재생된다.
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildVideoSearchTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("영상 검색"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 3단: 조건 | 결과 목록 | 재생기. 결과를 누르면 이 페이지 안에서 바로 재생된다
    // — 예전엔 이벤트 기록 페이지로 튕겨 보내서, 검색을 이어가려면 되돌아와야 했다.
    auto* body = new QHBoxLayout();
    body->setSpacing(12);

    // ── 좌: 검색 조건 ──
    auto* side = new QFrame();
    side->setObjectName("filterBar");
    side->setFixedWidth(248);
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(12, 12, 12, 12);
    sv->setSpacing(6);

    auto* cap = new QLabel(QStringLiteral("검색 조건"));
    cap->setObjectName("filterCap");
    sv->addWidget(cap);
    sv->addSpacing(4);

    auto* chCap = new QLabel(QStringLiteral("채널"));
    chCap->setObjectName("filterFieldCap");
    sv->addWidget(chCap);
    searchChannelCombo = new QComboBox();
    searchChannelCombo->setObjectName("searchChannelCombo");
    // 기본값 = 전체 채널 — 질문할 때 채널을 매번 고르지 않아도 전체에서 찾아준다.
    searchChannelCombo->addItem(QStringLiteral("전체 채널"), -1);
    for (int ch = 0; ch < 4; ++ch)
        searchChannelCombo->addItem(QStringLiteral("채널 %1").arg(ch + 1), ch);
    sv->addWidget(searchChannelCombo);
    sv->addSpacing(6);

    auto* qCap = new QLabel(QStringLiteral("질문"));
    qCap->setObjectName("filterFieldCap");
    sv->addWidget(qCap);
    searchQueryEdit = new QLineEdit();
    searchQueryEdit->setObjectName("searchQueryEdit");
    searchQueryEdit->setPlaceholderText(
        QStringLiteral("예: 어제 저녁에 낙상 있었어?"));
    sv->addWidget(searchQueryEdit);
    sv->addSpacing(8);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    searchButton = new QPushButton(QStringLiteral("검색"));
    searchButton->setObjectName("camPrimary");
    searchButton->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(searchButton, 1);
    auto* resetBtn = new QPushButton(QStringLiteral("초기화"));
    resetBtn->setObjectName("filterResetBtn");
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        searchQueryEdit->clear();
        searchChannelCombo->setCurrentIndex(0);
        searchResultList_->clear();
        searchResultList_->show();
        if (searchPlayer_) searchPlayer_->stop();
        if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(0);
        if (searchContext_) searchContext_->hide();
        if (searchMessage_) searchMessage_->hide();
        if (searchCountLabel_) searchCountLabel_->clear();
    });
    btnRow->addWidget(resetBtn);
    sv->addLayout(btnRow);
    sv->addSpacing(10);

    // 예시 질문 — 누르면 질문칸이 채워지고 바로 검색된다. 자연어 검색은 "뭐라고
    // 물어야 하는지"가 가장 큰 진입 장벽이라, 실제 통하는 문장을 눌러 보게 한다.
    auto* exCap = new QLabel(QStringLiteral("예시 질문"));
    exCap->setObjectName("filterFieldCap");
    sv->addWidget(exCap);
    const QString examples[3] = {
        QStringLiteral("어제 저녁에 낙상 있었어?"),
        QStringLiteral("이번 주에 침대에서 나간 적 있어?"),
        QStringLiteral("오늘 새벽에 무슨 일 있었어?"),
    };
    for (const QString& ex : examples) {
        auto* chip = new QPushButton(ex);
        chip->setObjectName("exampleChip");
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip, &QPushButton::clicked, this, [this, ex] {
            searchQueryEdit->setText(ex);
            sendSearchQuery();
        });
        sv->addWidget(chip);
    }
    sv->addStretch();
    body->addWidget(side, 0);

    // ── 중: 결과 목록 ──
    auto* mid = new QWidget();
    mid->setFixedWidth(300);
    auto* mv = new QVBoxLayout(mid);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(6);

    searchCountLabel_ = new QLabel();
    searchCountLabel_->setObjectName("logCount");
    mv->addWidget(searchCountLabel_);

    searchResultList_ = new QListWidget();
    searchResultList_->setObjectName("searchResultList");
    searchResultList_->setWordWrap(true);
    searchResultList_->setCursor(Qt::PointingHandCursor);
    // 한 번 클릭으로 재생 — 더블클릭까지 기다리게 하면 "왜 안 나오지"를 매번 겪는다.
    connect(searchResultList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
                if (!item) return;
                playSearchClip(item->data(Qt::UserRole).toString(),
                               item->data(Qt::UserRole + 1).toString());
            });
    mv->addWidget(searchResultList_, 1);

    // 목록으로 볼 수 없는 답변(안내문·오류)은 여기에 원문 그대로 띄운다.
    searchMessage_ = new QLabel();
    searchMessage_->setObjectName("searchMessage");
    searchMessage_->setWordWrap(true);
    searchMessage_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // 목록이 숨은 자리에서 라벨이 세로로 늘어나 문구가 한가운데 뜨지 않도록
    // 높이는 내용에 고정하고, 남는 공간은 아래 스트레치가 가져간다.
    searchMessage_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    searchMessage_->hide();
    mv->addWidget(searchMessage_);
    mv->addStretch();

    body->addWidget(mid, 0);

    // ── 우: 재생기 ──
    auto* right = new QVBoxLayout();
    right->setSpacing(6);

    searchContext_ = new QLabel();
    searchContext_->setObjectName("eventContext");
    searchContext_->setWordWrap(true);
    searchContext_->hide();
    right->addWidget(searchContext_);

    searchPlayerStack_ = new QStackedWidget();
    auto* hint = new QLabel(
        QStringLiteral("왼쪽에서 질문하고, 가운데 목록에서 기록을 고르면 "
                       "여기서 바로 재생됩니다."));
    hint->setObjectName("camHint");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    searchPlayerStack_->addWidget(hint);          // 0

    searchVideo_ = new QVideoWidget();
    searchVideo_->setObjectName("playbackVideo");
    searchPlayerStack_->addWidget(searchVideo_);  // 1

    auto* stageCard = new QFrame();
    stageCard->setObjectName("camStage");
    auto* scl = new QVBoxLayout(stageCard);
    scl->setContentsMargins(0, 0, 0, 0);
    scl->addWidget(searchPlayerStack_);
    right->addWidget(stageCard, 1);

    // 재생 컨트롤
    auto* ctl = new QFrame();
    ctl->setObjectName("transportBar");
    auto* cl = new QHBoxLayout(ctl);
    cl->setContentsMargins(10, 6, 10, 6);
    cl->setSpacing(8);
    searchPlayPause_ = new QPushButton(QStringLiteral("▶"));
    searchPlayPause_->setObjectName("transportBtn");
    searchPlayPause_->setCursor(Qt::PointingHandCursor);
    searchPlayPause_->setFixedSize(30, 26);
    cl->addWidget(searchPlayPause_);
    searchSeek_ = new QSlider(Qt::Horizontal);
    searchSeek_->setObjectName("blackboxSeek");
    cl->addWidget(searchSeek_, 1);
    searchTimeLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"));
    searchTimeLabel_->setObjectName("transportTime");
    cl->addWidget(searchTimeLabel_);
    right->addWidget(ctl);

    body->addLayout(right, 1);
    outer->addLayout(body, 1);

    connect(searchButton, &QPushButton::clicked, this, &MainWindow::sendSearchQuery);
    connect(searchQueryEdit, &QLineEdit::returnPressed, this, &MainWindow::sendSearchQuery);

    return panel;
}

// 서버 답변을 결과 목록으로 쪼갠다.
//
// 형식(video_search_module.cpp):
//   🔎 검색 결과 3건
//   (빈 줄)
//   · 2026-08-18 11:06 · 채널 4 · 낙상 · 전승현님
//     http://host:5501/ch3_...mp4
// "· "로 시작하면 새 결과, 들여쓴 http 줄은 직전 결과의 클립이다.
// 목록이 아닌 답변(안내문·"기록을 찾지 못했어요")은 message 로 넘긴다.
QVector<MainWindow::SearchHit> MainWindow::parseSearchReply(const QString& text,
                                                            QString* message)
{
    QVector<SearchHit> hits;
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QString::fromUtf8("· "))) {
            const QString rest = line.mid(2).trimmed();
            SearchHit hit;
            const int sep = rest.indexOf(QString::fromUtf8(" · "));
            if (sep > 0) {
                hit.when = rest.left(sep);
                hit.meta = rest.mid(sep + 3);
            } else {
                hit.when = rest;
            }
            hits.push_back(hit);
        } else if (!hits.isEmpty() && line.startsWith(QLatin1String("http")) &&
                   line.endsWith(QLatin1String(".mp4"))) {
            hits.back().url = line;
        }
    }

    if (hits.isEmpty() && message) *message = text.trimmed();
    return hits;
}

// 검색 결과 클립을 이 페이지 재생기에서 튼다.
void MainWindow::playSearchClip(const QString& url, const QString& context)
{
    if (searchContext_) {
        searchContext_->setText(context);
        searchContext_->show();
    }
    if (url.isEmpty()) {
        // 서버가 public_host 를 모르면 URL 없이 결과만 온다(모듈 주석 참고).
        if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(0);
        if (searchPlayer_) searchPlayer_->stop();
        return;
    }

    if (!searchPlayer_) {
        searchPlayer_ = new QMediaPlayer(this);
        searchPlayer_->setVideoOutput(searchVideo_);
        connect(searchPlayer_, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
            if (!searchSeeking_ && searchSeek_) searchSeek_->setValue(int(pos));
            if (searchTimeLabel_ && searchPlayer_) {
                const auto fmt = [](qint64 ms) {
                    return QStringLiteral("%1:%2")
                        .arg(ms / 60000, 2, 10, QLatin1Char('0'))
                        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
                };
                searchTimeLabel_->setText(QStringLiteral("%1 / %2")
                                              .arg(fmt(pos), fmt(searchPlayer_->duration())));
            }
        });
        connect(searchPlayer_, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
            if (searchSeek_) searchSeek_->setRange(0, int(qMax<qint64>(0, dur)));
        });
        connect(searchPlayer_, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
                    if (searchPlayPause_)
                        searchPlayPause_->setText(st == QMediaPlayer::PlayingState
                                                      ? QStringLiteral("⏸")
                                                      : QStringLiteral("▶"));
                });
        connect(searchPlayPause_, &QPushButton::clicked, this, [this] {
            if (!searchPlayer_) return;
            if (searchPlayer_->playbackState() == QMediaPlayer::PlayingState)
                searchPlayer_->pause();
            else
                searchPlayer_->play();
        });
        connect(searchSeek_, &QSlider::sliderPressed, this, [this] { searchSeeking_ = true; });
        connect(searchSeek_, &QSlider::sliderReleased, this, [this] {
            searchSeeking_ = false;
            if (searchPlayer_) searchPlayer_->setPosition(searchSeek_->value());
        });
    }

    if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(1);
    searchPlayer_->setSource(QUrl(url));
    searchPlayer_->play();
}

void MainWindow::sendSearchQuery()
{
    if (!searchChannelCombo || !searchQueryEdit || !searchButton || !searchResultList_)
        return;

    const QString query = searchQueryEdit->text().trimmed();
    if (query.isEmpty()) return;

    const int channel = searchChannelCombo->currentData().toInt();  // -1 = 전체 채널

    QTcpSocket* sock = nullptr;
    if (channel >= 0) {
        sock = socketForChannel(channel);
    } else {
        // 전체 채널 검색 — DB를 두 Pi가 공유하므로(2-Pi 분할) 아무 Pi에나 물어봐도
        // 전체 결과가 나온다. 연결된 소켓 중 먼저 찾은 것(Pi A 우선)으로 보낸다.
        for (int i = 0; i < kNumServers; ++i) {
            if (sockets[i]->state() == QAbstractSocket::ConnectedState) {
                sock = sockets[i];
                break;
            }
        }
    }
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) {
        searchResultList_->clear();
        searchResultList_->hide();
        searchCountLabel_->clear();
        searchMessage_->setText(QStringLiteral("영상 서버에 연결되어 있지 않습니다."));
        searchMessage_->show();
        return;
    }

    const QByteArray q = query.toUtf8();
    const int len = qMin(q.size(), kSearchQueryMax);

    dbj_ctrl_header_t h;
    h.magic = kCtrlMagic;
    h.version = 0x01;
    h.type = kCtrlSearchQuery;
    h.channel = (channel < 0) ? kChannelAll : static_cast<uint8_t>(channel);
    h.point_count = 0;
    h.reserved = static_cast<uint16_t>(len);

    QByteArray pkt;
    pkt.append(reinterpret_cast<const char*>(&h), sizeof(h));
    pkt.append(q.constData(), len);
    sock->write(pkt);
    sock->flush();

    searchButton->setEnabled(false);
    searchResultList_->clear();
    searchResultList_->show();
    searchCountLabel_->setText(QStringLiteral("검색 중…"));
    searchMessage_->hide();

    // Gemini+DB 왕복이 수 초 걸릴 수 있어, 응답이 안 오면 버튼이 영원히 잠기지
    // 않도록 넉넉한 시간 후 자동 복구(서버 curl 타임아웃 20초보다 여유 있게).
    QTimer::singleShot(25000, this, [this]() {
        if (searchButton && !searchButton->isEnabled()) {
            searchButton->setEnabled(true);
            if (searchMessage_)
                searchMessage_->setText(
                    QStringLiteral("응답이 없어요. 다시 시도해 주세요."));
        }
    });
}

void MainWindow::onSearchResultReceived(int /*channel*/, const QString& text)
{
    if (searchButton) searchButton->setEnabled(true);
    if (!searchResultList_) return;

    QString message;
    const QVector<SearchHit> hits = parseSearchReply(text, &message);

    searchResultList_->clear();
    if (hits.isEmpty()) {
        // 목록이 아닌 답변(안내문·"기록을 찾지 못했어요")은 원문 그대로 보여준다.
        // 빈 목록 상자를 남겨 두면 안내문이 그 아래로 밀려 붙어 어색하다 —
        // 둘 중 하나만 자리를 차지한다.
        if (searchCountLabel_) searchCountLabel_->clear();
        searchResultList_->hide();
        if (searchMessage_) {
            searchMessage_->setText(message);
            searchMessage_->show();
        }
        return;
    }

    searchResultList_->show();
    if (searchMessage_) searchMessage_->hide();
    for (const SearchHit& h : hits) {
        auto* item = new QListWidgetItem(
            h.meta.isEmpty() ? h.when
                             : QStringLiteral("%1\n%2").arg(h.when, h.meta));
        item->setData(Qt::UserRole, h.url);
        item->setData(Qt::UserRole + 1,
                      h.meta.isEmpty() ? h.when
                                       : QStringLiteral("%1  ·  %2").arg(h.when, h.meta));
        // 클립이 없는 기록은 눌러도 재생할 게 없다 — 미리 알려 준다.
        if (h.url.isEmpty()) {
            item->setToolTip(QStringLiteral("저장된 클립이 없는 기록입니다"));
            item->setForeground(QColor(QString::fromLatin1(kTextSub)));
        }
        searchResultList_->addItem(item);
    }
    if (searchCountLabel_)
        searchCountLabel_->setText(QStringLiteral("%1건").arg(hits.size()));
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
    basicForm->addRow(QStringLiteral("호실"),        makeField("예: 101", editRoom));
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

void MainWindow::connectToServer()
{
    // 끊겨 있는 서버 소켓만 골라 재접속 (일부 Pi만 끊긴 경우도 처리)
    for (int i = 0; i < kNumServers; ++i) {
        if (sockets[i]->state() != QAbstractSocket::UnconnectedState) continue;
        buffers[i].clear();  // 이전 연결의 파싱 잔여물 폐기
        const QString host = serverHost(i);

        // TLS/평문은 CA 파일 유무로 가른다 — MqttQtManager::startConnection()의
        // m_caPath.isEmpty() 판단과 같은 패턴. certs/ca.crt를 배포했다는 것 자체가
        // "이 Pi들은 stream_cert_path를 켜서 TLS로 띄웠다"는 선언으로 본다. 아직
        // 인증서를 안 놓은 개발 환경/미전환 Pi에서는 파일이 없어 기존처럼 평문 접속.
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
            sockets[i]->setSslConfiguration(conf);
            sockets[i]->connectToHostEncrypted(host, kServerPort);
            qDebug() << "영상 서버 접속 시도(TLS):" << host << ":" << kServerPort;
        } else {
            sockets[i]->connectToHost(QHostAddress(host), kServerPort);
            qDebug() << "영상 서버 접속 시도(평문):" << host << ":" << kServerPort;
        }
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

    metrics_ = ReportMetrics{};   // 이번 조회 결과로 새로 채운다(이전 값 잔류 방지)

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
            metrics_.careSec   = q.value(0).toInt();
            metrics_.careCount = q.value(1).toInt();
            tileCareVal->setText(humanDuration(q.value(0).toInt()));
            const QDateTime last = q.value(2).toDateTime();
            if (last.isValid()) metrics_.careLast = last.toString(QStringLiteral("HH:mm"));
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
            metrics_.lyingSec   = q.value(0).toInt();
            metrics_.lyingCount = q.value(1).toInt();
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
            metrics_.steps = steps; metrics_.activeMin = activeMin;
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
        QVector<int> stepsByHour(24, 0);
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
            }
        } else {
            qDebug() << "시간별 활동량 조회 실패:" << q.lastError().text();
        }
        activityChart->setData(stepsByHour);
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
        metrics_.eventTotal  = total;
        metrics_.eventDetail = parts.join(QStringLiteral(" · "));
        tileEventVal->setText(total > 0 ? QStringLiteral("%1회").arg(total)
                                        : QStringLiteral("—"));
        tileEventSub->setText(parts.isEmpty() ? QStringLiteral("이벤트 없음")
                                              : parts.join(QStringLiteral(" · ")));
    }

    // 이름·위치는 탭 전환에서 이미 정해진 값을 그대로 쓴다(다시 조회하지 않는다).
    for (int i = 0; i < residentTabBtns.size(); ++i)
        if (residentTabIds.value(i) == rid) metrics_.residentName = residentTabBtns[i]->text();
    if (reportResidentMeta) metrics_.residentMeta = reportResidentMeta->text();
    metrics_.valid = true;
    loadCachedSummary();   // 이 날짜·입소자의 저장된 요약을 표시(API 호출 없음)
}

// ═══════════════════════════════════════════════════════════
//  일일 리포트 PDF 내보내기 — 선택한 날짜·입소자 한 장
//
//  HTML(QTextDocument) → QPdfWriter 로 만든다. QPainter 로 직접 좌표를 찍으면
//  페이지 넘김을 손으로 관리해야 하는데, HTML 은 표·여백을 알아서 흘려주고
//  한글 폰트도 자동으로 임베딩된다.
//  ★ QPdfWriter 는 Qt::Gui 에 있어 모듈 추가가 필요 없다(PrintSupport 불필요).
// ═══════════════════════════════════════════════════════════
void MainWindow::exportReportPdf()
{
    if (!metrics_.valid || reportResidentId_ <= 0) {
        QMessageBox::information(this, QStringLiteral("PDF 내보내기"),
                                 QStringLiteral("먼저 날짜와 입소자를 선택해 주세요."));
        return;
    }

    const QString dateStr = reportDate_.toString(QStringLiteral("yyyy-MM-dd"));
    const QString suggest = QStringLiteral("%1_%2_리포트.pdf")
                                .arg(metrics_.residentName.isEmpty()
                                         ? QStringLiteral("입소자") : metrics_.residentName)
                                .arg(dateStr);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("일일 리포트 저장"),
        QDir::homePath() + QLatin1Char('/') + suggest,
        QStringLiteral("PDF 파일 (*.pdf)"));
    if (path.isEmpty()) return;   // 사용자가 취소

    // ── PDF 장치를 먼저 만든다 ──
    // ★ 해상도를 300dpi 로 낮춰 잡는다. QPdfWriter 기본값은 1200dpi 인데, 그러면
    //   본문 폭이 device 픽셀로 ~9900 이 되어 이미지 폭 같은 픽셀 값을 가늠하기
    //   어렵다. 글자는 pt(해상도 무관)라 300dpi 로도 벡터로 또렷하게 나온다.
    // ★ 이미지 폭을 여기서 구해 HTML 에 넣어야 한다. HTML 의 width 는 device
    //   픽셀이라, 본문 폭을 모르고 700 같은 상수를 쓰면 종이 한구석에만 찍힌다.
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setResolution(300);
    writer.setTitle(QStringLiteral("%1 %2 일일 리포트").arg(metrics_.residentName, dateStr));
    // 실제로 인쇄되는 영역(여백 제외). writer.width() 는 여백을 포함한 종이 전체라
    // 이미지 폭 기준으로 쓰면 오른쪽이 잘린다.
    const QRect bodyPx = writer.pageLayout().paintRectPixels(writer.resolution());


    // ── 그래프를 이미지로 ──
    // ★ 크기: writer.width() 는 여백을 포함한 종이 전체 폭이라 그대로 쓰면 본문을
    //   넘어 오른쪽이 잘린다. paintRectPixels() 로 "실제 인쇄되는 영역"을 받아 쓴다.
    //   가로세로를 둘 다 지정해 비율이 흐트러지지 않게 하고, 세로가 페이지의 1/3 을
    //   넘으면 세로 기준으로 다시 줄여 그래프 혼자 한 장을 차지하지 않게 한다.
    // ★ 색: 화면은 다크 테마일 수 있는데 종이는 흰색이다. 그대로 뜨면 어두운 상자가
    //   찍히고 회색 눈금이 잘 안 보인다. 그리는 동안만 팔레트를 라이트로 바꿔
    //   렌더하고 곧바로 되돌린다(같은 스레드에서 동기 렌더라 화면에는 영향 없다).
    // ★ 해상도: grab() 은 화면 dpi 로 떠서 300dpi 종이에선 흐리다. 3배로 렌더한다.
    QString chartHtml;
    if (activityChart && activityChart->width() > 0) {
        constexpr int kScale = 3;
        QPixmap shot(activityChart->size() * kScale);

        const Palette saved{kBgDeep, kPanel, kCard, kBorder, kTextMain, kTextSub,
                            kAccent, kOnAccent, kNormal, kWarn, kHigh, kCritical,
                            kInfo, kSelect, kOnSelect};
        applyPalette(kLight);
        shot.fill(Qt::white);
        QPainter pp(&shot);
        pp.scale(kScale, kScale);
        activityChart->render(&pp, QPoint(), QRegion(), QWidget::DrawChildren);
        pp.end();
        applyPalette(saved);
        activityChart->update();   // 화면은 원래 팔레트로 다시 그린다

        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (shot.save(&buf, "PNG")) {
            // ★ HTML 의 width/height 는 96dpi 논리 픽셀로 읽히고, 인쇄할 때 장치
            //   해상도만큼 다시 확대된다. 300dpi 면 3.125 배다. bodyPx(장치 픽셀)를
            //   그대로 넣으면 그 배율로 또 커져 종이 밖으로 나간다 — 나눠서 넣는다.
            const double dpiScale = writer.resolution() / 96.0;
            int w = int(bodyPx.width() * 0.98 / dpiScale);
            int h = shot.width() > 0 ? w * shot.height() / shot.width() : 0;
            const int hMax = int(bodyPx.height() / 3.0 / dpiScale);
            if (h > hMax && h > 0) { w = w * hMax / h; h = hMax; }
            chartHtml = QStringLiteral(
                "<p style='margin-top:26px'><b>시간별 활동량</b></p>"
                "<p style='color:#5C6B78; margin-top:2px'>"
                "막대 하나가 한 시간 동안 걸은 걸음 수입니다. "
                "가장 활발했던 시간대는 진하게 표시됩니다.</p>"
                "<img src='data:image/png;base64,%1' width='%2' height='%3'>")
                .arg(QString::fromLatin1(png.toBase64()))
                .arg(w).arg(h);
        }
    }
    if (chartHtml.isEmpty()) {
        // 그래프를 못 그렸을 때(위젯이 없거나 이 날짜에 걸음 데이터가 없어 폭이 0일 때) —
        // 캡션만 있고 그 아래가 텅 비면 페이지가 어색해 보인다. 그래프가 있었을 때와
        // 비슷한 높이의 빈 칸을 만들고 안내문을 그 칸 한가운데(가로·세로 모두)에 둔다.
        const double dpiScale = writer.resolution() / 96.0;
        const int hMax = int(bodyPx.height() / 3.0 / dpiScale);
        chartHtml = QStringLiteral(
            "<p style='margin-top:26px'><b>시간별 활동량</b></p>"
            "<table width='100%' cellspacing='0' cellpadding='0' border='0'>"
            "<tr><td align='center' valign='middle' height='%1' "
            "style='color:#8B98A5'>표시할 활동량 데이터가 없습니다</td></tr></table>")
            .arg(hMax);
    }

    // ── 이벤트 목록 ──
    // 숫자만 있으면 "새벽 3시"인지 "낮 3시"인지 알 수 없다. 시각이 있어야
    // 보호자가 읽을 수 있는 문서가 된다.
    QString eventRows;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT TIME(occurred_at), event_type, source, confirmed_at FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(reportResidentId_);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(1).toString();
                const QString label = t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                                    : t == QLatin1String("EGRESS") ? QStringLiteral("침상이탈")
                                                                   : QStringLiteral("생체신호 이상");
                eventRows += QStringLiteral(
                    "<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                    .arg(q.value(0).toString().left(5), label,
                         q.value(2).toString() == QLatin1String("WEARABLE")
                             ? QStringLiteral("웨어러블") : QStringLiteral("카메라"),
                         q.value(3).isNull() ? QStringLiteral("미확인")
                                             : QStringLiteral("확인됨"));
            }
        }
    }
    if (eventRows.isEmpty())
        eventRows = QStringLiteral("<tr><td colspan='4'>이벤트 없음</td></tr>");

    // ── AI 요약 (있을 때만) ──
    // PDF 는 AI 요약에 의존하지 않는다. 생성해 둔 적이 있으면 넣고, 없으면 그 칸 없이
    // 나온다 — 요약이 없다고 리포트를 못 뽑으면 안 되기 때문이다.
    // ★ 회색 상자로 감싸 위쪽 수치와 시각적으로 떼어 놓는다. 위 숫자는 기계가 센
    //   값이고 이 문단은 생성된 글이라, 감사(監査) 상황에서 그 구분이 중요하다.
    QString summaryHtml;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT summary_text, model, generated_at FROM daily_reports "
            "WHERE report_date=? AND resident_id=?"));
        q.addBindValue(reportDate_);
        q.addBindValue(reportResidentId_);
        if (q.exec() && q.next() && !q.value(0).toString().trimmed().isEmpty()) {
            summaryHtml = QStringLiteral(
                "<p style='margin-top:24px'><b>요약</b></p>"
                "<table width='100%' cellspacing='0' cellpadding='10' border='1'"
                "       bordercolor='#DCE4EC'><tr bgcolor='#F0F4F8'><td>%1"
                "<br><span style='color:#8B98A5; font-size:8pt'>"
                "%2 자동 생성 · 위 수치를 근거로 작성되었습니다</span>"
                "</td></tr></table>")
                .arg(q.value(0).toString().toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                   QStringLiteral("<br>")),
                     q.value(2).toDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
    }

    const QString html = QStringLiteral(R"HTML(
<html><body style="font-family:'맑은 고딕',sans-serif; font-size:11pt; color:#1E2A32; line-height:155%">

<table width="100%" cellspacing="0" cellpadding="0" border="0">
  <tr>
    <td><span style="font-size:10pt; color:#5C6B78; letter-spacing:2px">Carenet 요양원 통합 모니터링</span>
        <h1 style="font-size:26pt; margin:2px 0 0 0">일일 리포트</h1></td>
    <td align="right" valign="bottom">
        <span style="font-size:9pt; color:#8B98A5">문서번호 %17<br>생성 %15</span></td>
  </tr>
</table>
<hr style="border:3px solid #C25A10; margin-top:6px">

<table width="100%" cellspacing="0" cellpadding="9" border="0" style="margin-top:10px">
  <tr bgcolor="#FBEAD9">
    <td width="14%"><b>성명</b></td><td width="36%">%1</td>
    <td width="14%"><b>대상일</b></td><td width="36%">%3</td>
  </tr>
  <tr>
    <td><b>구분</b></td><td>%2</td>
    <td><b>작성</b></td><td>자동 기록 (담당자 입력 없음)</td>
  </tr>
</table>

<p style="margin-top:20px; font-size:13pt"><b style="color:#C25A10">1.</b> <b>요약 지표</b></p>
<table width="100%" cellspacing="0" cellpadding="11" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
  <tr bgcolor="#FBEAD9">
    <th align="left" width="25%">누워있는 시간</th><th align="left" width="25%">활동량</th>
    <th align="left" width="25%">케어시간</th><th align="left" width="25%">이벤트</th>
  </tr>
  <tr>
    <td><b style="font-size:17pt; color:#C25A10">%4</b><br><span style="color:#5C6B78">재실 %5회</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%6걸음</b><br><span style="color:#5C6B78">활동 %7분</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%8</b><br><span style="color:#5C6B78">%9회 %10</span></td>
    <td><b style="font-size:17pt; color:#C25A10">%11회</b><br><span style="color:#5C6B78">%12</span></td>
  </tr>
  <tr style="color:#5C6B78">
    <td>침대에 계셨던 시간의 합계</td>
    <td>웨어러블이 센 걸음 수와 실제로 움직인 시간</td>
    <td>요양사가 병실에 머문 시간 중 침대에 계셨던 시간</td>
    <td>낙상 · 침상이탈 · 생체신호 이상 감지 횟수</td>
  </tr>
</table>

<p style="margin-top:24px; font-size:13pt"><b style="color:#C25A10">2.</b> <b>시간별 활동량</b></p>
%13

<p style="page-break-before:always; font-size:13pt"><b style="color:#C25A10">3.</b> <b>이벤트 내역</b></p>
<p style="color:#5C6B78; margin-top:2px">
  감지된 시각과 종류입니다. ‘확인’은 관제 담당자가 영상을 확인해 처리한 건입니다.
</p>
<table width="100%" cellspacing="0" cellpadding="10" border="1" bordercolor="#DCE4EC" style="margin-top:4px">
  <tr bgcolor="#FBEAD9"><th align="left" width="14%">시각</th><th align="left" width="30%">종류</th>
      <th align="left" width="28%">감지 방식</th><th align="left" width="28%">확인 여부</th></tr>
  %14
</table>
%16

<p style="margin-top:30px; font-size:10pt"><b>측정 방식 안내</b></p>
<table width="100%" cellspacing="0" cellpadding="8" border="1" bordercolor="#DCE4EC">
  <tr><td style="color:#5C6B78; font-size:9pt; line-height:150%">
    누워있는 시간과 케어시간은 병실 카메라가 침대 영역을 기준으로 판단합니다.
    침대를 벗어나 의자 등에서 보낸 시간은 포함되지 않을 수 있습니다.<br>
    활동량과 생체신호는 손목 웨어러블에서 수집합니다.<br>
    기기 특성상 실제와 차이가 있을 수 있으며, <b>의료적 판단의 근거로 사용하지 마십시오.</b>
  </td></tr>
</table>
<p style="margin-top:14px; color:#8B98A5; font-size:8pt">
  Carenet 요양원 통합 모니터링 &middot; 이 문서는 시스템이 자동 생성했습니다.
</p>
</body></html>)HTML")


        // ★ QString::arg 의 다중 인자 오버로드는 최대 9개다. 17개를 한 번에 넘기면
        //   컴파일이 안 되므로 9 + 8 로 나눈다. 낮은 번호 placeholder 부터
        //   순서대로 채워지므로 이렇게 쪼개도 결과는 같다.
        .arg(metrics_.residentName, metrics_.residentMeta,
             QStringLiteral("%1 (%2)").arg(reportDate_.toString(QStringLiteral("yyyy년 M월 d일")),
                                           koreanDow(reportDate_)),
             humanDuration(metrics_.lyingSec), QString::number(metrics_.lyingCount),
             QString::number(metrics_.steps), QString::number(metrics_.activeMin),
             humanDuration(metrics_.careSec), QString::number(metrics_.careCount))
        .arg(metrics_.careLast.isEmpty() ? QString()
                                         : QStringLiteral("· 마지막 %1").arg(metrics_.careLast),
             QString::number(metrics_.eventTotal),
             metrics_.eventDetail.isEmpty() ? QStringLiteral("이벤트 없음")
                                            : metrics_.eventDetail,
             chartHtml, eventRows,
             QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             summaryHtml,
             // 문서번호 — 같은 입소자·같은 날짜면 항상 같은 값이라 재발행해도 안 바뀐다.
             QStringLiteral("%1-%2").arg(reportDate_.toString(QStringLiteral("yyyyMMdd")))
                                    .arg(reportResidentId_, 4, 10, QLatin1Char('0')));

    // ★ doc.setPageSize 를 직접 주지 않는다. 주면 QTextDocument 가 화면 DPI 로
    //   배치된 채 고해상도 페이지에 그대로 얹혀 글자가 1/10 크기로 나온다.
    //   비워 두면 print() 가 문서를 복제해 인쇄 장치 DPI 로 다시 배치해 준다.
    QTextDocument doc;
    doc.setHtml(html);
    doc.print(&writer);

    QMessageBox::information(this, QStringLiteral("PDF 내보내기"),
                             QStringLiteral("저장했습니다.\n%1").arg(path));
}

// ═══════════════════════════════════════════════════════════
//  AI 요약 — Gemini 로 리포트 한 문단 만들기
//
//  ★ 숫자는 AI 가 만들지 않는다. SQL 이 만든 집계값(metrics_)을 그대로 넘기고
//    AI 는 문장만 쓴다. 리포트는 요양 기록물이라 같은 날짜를 두 번 열었을 때
//    값이 달라지면 안 되고, "이 낙상 1회가 어디서 나왔나"에 답할 수 있어야 한다.
//  ★ 결과는 daily_reports 에 캐시한다. 같은 이유(재현성) + API 비용·지연 절감.
// ═══════════════════════════════════════════════════════════


namespace {
// Gemini 모델.
// ★ "gemini-flash-latest" 같은 별칭 대신 구체 버전을 쓴다. 별칭은 트래픽이 몰리는
//   공용 풀로 라우팅돼 503(This model is currently experiencing high demand)이 잦다.
//   버전을 못박으면 대체로 더 안정적으로 붙는다. 대신 모델이 은퇴하면 직접 올려야
//   하므로, 404(model not found)가 뜨면 이 값을 최신 버전으로 바꾼다 — 다행히
//   Google 이 404 응답에 "use models/... instead" 로 대체 모델을 알려준다.
// 서버(gemini_client / cameras.conf)도 별도로 모델을 갖고 있다 — 바꿀 땐 양쪽 같이.
constexpr const char* kGeminiModel = "gemini-3.6-flash";
}  // namespace
// 키 우선순위: 소스에 박은 값 → 빌드에 박은 값(DABOYIJO_GEMINI_KEY) → QSettings.
//
// ⚠ 이 저장소는 공개(public) GitHub repo다. 아래 kHardcodedGeminiKey 에 실제 키를
//   적고 커밋/푸시하면 그 순간 키가 인터넷에 노출되고, 히스토리에서 지워도 이미
//   남에게 긁혀갔을 수 있다 — 과금성 남용으로 이어질 수 있다는 점을 알고 쓸 것.
//   (요청에 따라 CMake 설정 없이 바로 쓸 수 있게 이 자리에 하드코딩 경로를 열어둠)
namespace {
constexpr const char* kHardcodedGeminiKey = "AIzaSyCfWoiYaFBOngxCAJDkt5z5t-FCQEhHp_w";  // 여기에 실제 Gemini API 키를 적으세요
}  // namespace
QString MainWindow::geminiApiKey() const
{
    const QString hardcoded = QString::fromLatin1(kHardcodedGeminiKey).trimmed();
    if (!hardcoded.isEmpty()) {
        qDebug() << "[AI] 소스 하드코딩 키 사용, 길이:" << hardcoded.size();
        return hardcoded;
    }
#ifdef DABOYIJO_GEMINI_KEY
    const QString baked = QString::fromLatin1(DABOYIJO_GEMINI_KEY).trimmed();
    // 키 자체는 절대 찍지 않는다 — 길이만으로 "정의가 왔는지/비었는지"가 갈린다.
    qDebug() << "[AI] 빌드 주입 키 길이:" << baked.size();
    if (!baked.isEmpty()) return baked;
#else
    qDebug() << "[AI] DABOYIJO_GEMINI_KEY 매크로 자체가 정의되지 않음 — CMake 재실행 필요";
#endif
    const QString fromSettings =
        QSettings().value(QStringLiteral("ai/geminiKey")).toString().trimmed();
    qDebug() << "[AI] QSettings 폴백 키 길이:" << fromSettings.size();
    return fromSettings;
}

void MainWindow::setSummaryText(const QString& text, bool cached)
{
    if (!summaryLabel) return;
    if (text.isEmpty()) {
        summaryLabel->setText(
            QStringLiteral("요약이 아직 없습니다. [요약] 버튼을 눌러 생성하세요."));
        summaryLabel->setProperty("empty", true);
    } else {
        // 생성된 문장임을 눈에 보이게 표시한다 — 위쪽 숫자는 기계가 센 값이고
        // 이 문단은 AI 가 쓴 글이라는 구분이 감사(監査) 상황에서 중요하다.
        summaryLabel->setText(cached ? text
                                     : QStringLiteral("%1").arg(text));
        summaryLabel->setProperty("empty", false);
    }
    summaryLabel->style()->unpolish(summaryLabel);
    summaryLabel->style()->polish(summaryLabel);
    if (summaryBtn)
        summaryBtn->setText(text.isEmpty() ? QStringLiteral("요약")
                                           : QStringLiteral("요약 다시 생성"));
}

// 날짜·입소자가 바뀔 때마다 캐시를 먼저 보여준다(API 호출 없음).
void MainWindow::loadCachedSummary()
{
    if (!summaryLabel) return;
    if (reportResidentId_ <= 0) { setSummaryText(QString(), true); return; }

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT summary_text FROM daily_reports WHERE report_date=? AND resident_id=?"));
    q.addBindValue(reportDate_);
    q.addBindValue(reportResidentId_);
    setSummaryText((q.exec() && q.next()) ? q.value(0).toString() : QString(), true);
}

void MainWindow::requestAiSummary()
{
    if (summaryBusy_) return;                       // 응답 오기 전 중복 클릭 방지
    if (!metrics_.valid || reportResidentId_ <= 0) {
        QMessageBox::information(this, QStringLiteral("요약"),
                                 QStringLiteral("먼저 날짜와 입소자를 선택해 주세요."));
        return;
    }
    const QString key = geminiApiKey();
    if (key.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("요약"),
            QStringLiteral("요약 기능이 이 빌드에 설정되지 않았습니다.\n\n"
                           "빌드할 때 DABOYIJO_GEMINI_KEY 를 지정해야 합니다.\n"
                           "(Qt Creator: 프로젝트 > 빌드 설정 > CMake)\n\n"
                           "나머지 리포트 기능은 그대로 사용할 수 있습니다."));
        return;
    }

    // ── 프롬프트: 집계 수치만 ──
    // 원본 로그(개별 이벤트 행, 분당 걸음)는 보내지 않는다. 요약에 필요하지도 않고
    // 개인정보를 넓게 노출할 이유도 없다.
    QString events;
    {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT TIME(occurred_at), event_type FROM events "
            "WHERE resident_id=? AND DATE(occurred_at)=? ORDER BY occurred_at"));
        q.addBindValue(reportResidentId_);
        q.addBindValue(reportDate_);
        if (q.exec()) {
            while (q.next()) {
                const QString t = q.value(1).toString();
                events += QStringLiteral("%1 %2, ").arg(
                    q.value(0).toString().left(5),
                    t == QLatin1String("FALL")   ? QStringLiteral("낙상")
                  : t == QLatin1String("EGRESS") ? QStringLiteral("침상이탈")
                                                 : QStringLiteral("생체신호 이상"));
            }
        }
    }
    if (events.isEmpty()) events = QStringLiteral("없음");

    const QString prompt = QStringLiteral(
        "당신은 요양원 간호기록을 쓰는 담당자입니다. 아래는 입소자 한 명의 하루 기록입니다.\n"
        "이 수치만 근거로 보호자가 읽을 3~4문장 요약을 한국어로 작성하세요.\n\n"
        "[규칙]\n"
        "- 주어진 수치 외의 어떤 숫자도 만들어내지 마세요. 특히 '평소 대비', '지난주보다' 같은\n"
        "  비교는 비교할 자료가 없으므로 절대 쓰지 마세요.\n"
        "- 의학적 진단이나 처방을 하지 마세요. 관찰된 사실과 그 의미만 서술하세요.\n"
        "- 이벤트가 있었다면 시각과 함께 먼저 언급하세요.\n"
        "- 문장만 출력하세요. 제목·머리말·목록 기호는 넣지 마세요.\n\n"
        "[기록]\n"
        "날짜: %1\n입소자: %2 (%3)\n"
        "누워있던 시간: %4 (재실 %5회)\n"
        "활동량: %6걸음 (움직인 시간 %7분)\n"
        "요양사 케어: %8 (%9회)\n"
        "이벤트: %10")
        .arg(reportDate_.toString(QStringLiteral("yyyy년 M월 d일")),
             metrics_.residentName, metrics_.residentMeta,
             humanDuration(metrics_.lyingSec), QString::number(metrics_.lyingCount),
             QString::number(metrics_.steps), QString::number(metrics_.activeMin),
             humanDuration(metrics_.careSec), QString::number(metrics_.careCount))
        .arg(events);

    // ── Gemini 호출 ──
    // 서버(gemini_client.cpp)와 같은 엔드포인트를 쓴다. 모델을 바꾸려면 양쪽을 같이.
    QJsonObject part;   part.insert(QStringLiteral("text"), prompt);
    QJsonObject content; content.insert(QStringLiteral("parts"), QJsonArray{part});
    QJsonObject body;    body.insert(QStringLiteral("contents"), QJsonArray{content});

    QNetworkRequest req(QUrl(QStringLiteral(
        "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
            .arg(QLatin1String(kGeminiModel))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", key.toUtf8());

    summaryBusy_ = true;
    if (summaryBtn) summaryBtn->setEnabled(false);
    if (summaryLabel) summaryLabel->setText(QStringLiteral("요약을 생성하는 중입니다…"));

    // 응답이 오는 사이 사용자가 날짜·입소자를 바꿀 수 있다. 그때 엉뚱한 리포트에
    // 요약이 붙지 않도록 요청 시점의 대상을 캡처해 두고 도착 시 비교한다.
    const QDate reqDate = reportDate_;
    const int   reqRid  = reportResidentId_;

    auto* nam = new QNetworkAccessManager(this);
    QNetworkReply* reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, nam, reqDate, reqRid]() {
        reply->deleteLater();
        nam->deleteLater();
        summaryBusy_ = false;
        if (summaryBtn) summaryBtn->setEnabled(true);

        if (reply->error() != QNetworkReply::NoError) {
            // Gemini 는 실패해도 본문에 {"error":{"message":"..."}} 를 담아 보낸다.
            // 상태 코드만 보면 "503" 밖에 안 나와 원인을 알 수 없으므로 본문을 읽는다.
            const QByteArray raw = reply->readAll();
            const QJsonObject err = QJsonDocument::fromJson(raw).object()
                                        .value(QStringLiteral("error")).toObject();
            const QString detail = err.value(QStringLiteral("message")).toString();
            const int code = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();

            // 503/429 는 모델 과부하·쿼터라 대개 잠시 뒤 다시 누르면 된다.
            const QString hint =
                (code == 503) ? QStringLiteral(" (모델 과부하 — 잠시 후 다시 시도해 보세요)")
              : (code == 429) ? QStringLiteral(" (요청 한도 초과 — 잠시 후 다시 시도해 보세요)")
              : (code == 400 || code == 403)
                    ? QStringLiteral(" (API 키를 확인해 주세요)")
                    : QString();

            const QString msg = detail.isEmpty() ? reply->errorString() : detail;
            qDebug() << "AI 요약 실패:" << code << msg << raw;
            if (summaryLabel)
                summaryLabel->setText(QStringLiteral("요약 생성 실패: %1%2").arg(msg, hint));
            return;
        }

        // candidates[0].content.parts[0].text 를 꺼낸다(서버 구현과 같은 경로).
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray cands = root.value(QStringLiteral("candidates")).toArray();
        QString text;
        if (!cands.isEmpty()) {
            const QJsonArray parts = cands.first().toObject()
                                          .value(QStringLiteral("content")).toObject()
                                          .value(QStringLiteral("parts")).toArray();
            if (!parts.isEmpty())
                text = parts.first().toObject().value(QStringLiteral("text")).toString().trimmed();
        }
        if (text.isEmpty()) {
            if (summaryLabel)
                summaryLabel->setText(QStringLiteral("요약 응답을 해석하지 못했습니다."));
            return;
        }

        // 캐시는 요청 시점 대상에 저장한다(그 사이 화면이 바뀌었어도 기록은 제자리로).
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "INSERT INTO daily_reports (report_date, resident_id, summary_text, model, generated_at) "
            "VALUES (?,?,?,?,NOW()) "
            "ON DUPLICATE KEY UPDATE summary_text=VALUES(summary_text), "
            "model=VALUES(model), generated_at=VALUES(generated_at)"));
        q.addBindValue(reqDate);
        q.addBindValue(reqRid);
        q.addBindValue(text);
        q.addBindValue(QLatin1String(kGeminiModel));
        if (!q.exec()) qDebug() << "AI 요약 캐시 저장 실패:" << q.lastError().text();

        // 화면이 아직 그 리포트를 보고 있을 때만 표시한다.
        if (reqDate == reportDate_ && reqRid == reportResidentId_)
            setSummaryText(text, false);
    });
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

        // ── 영상검색 결과 패킷 (🔍, 페이로드 있음) — 스킵 금지 ──
        if (magic == kSearchMagic) {
            if (buffer.size() < (int)sizeof(dbj_search_result_header_t))
                break;  // 헤더가 덜 옴 — 다음 readyRead 대기
            dbj_search_result_header_t sh;
            memcpy(&sh, buffer.constData(), sizeof(sh));

            const qint64 total = static_cast<qint64>(sizeof(sh)) + sh.text_len;
            if (buffer.size() < total)
                break;  // 텍스트가 아직 덜 옴 — 다음 readyRead 대기

            const QByteArray textBytes(buffer.constData() + sizeof(sh),
                                       static_cast<int>(sh.text_len));
            buffer.remove(0, static_cast<int>(total));

            onSearchResultReceived(sh.channel, QString::fromUtf8(textBytes));
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
        const bool wasLive = channelViews[ch]->live();
        channelViews[ch]->setLive(true);   // 프레임 도착 → LIVE 표시등 점등
        // 죽어 있던 채널이 살아난 순간에만 트리를 다시 칠한다 — 매 프레임 갱신하면
        // 초당 수십 번 트리 전체를 다시 그리게 된다.
        if (!wasLive) refreshResourceTree();
        lastFrameMs_[ch] = QDateTime::currentMSecsSinceEpoch();  // checkChannelHealth() 판정용
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
        if (imgWipe_ && roiEditChannel == ch && cameraSettingsVisible() &&
            camMode_ == QStringLiteral("이미지"))
            setImagePreviewFrame(pix);   // 첫 프레임에서 상자 비율도 함께 잡힌다
    }
}

// ═══════════════════════════════════════════════════════════
//  낙상 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
// 하단 타임라인에 이벤트 눈금을 하나 찍는다. 이벤트 로그(logTable)와 별개로
// 가벼운 목록을 따로 들고 있는다 — 타임라인은 "언제 무슨 색"만 알면 되고,
// 표를 매번 훑으면 그릴 때마다 전체 로그를 파싱하게 된다.
void MainWindow::pushTimelineEvent(int channel, qint64 atMs, const QColor& color)
{
    if (channel < 0 || channel >= 4) return;
    timelineEvents_.push_back({atMs, channel, color});
    // NVR 보존기간(12시간)보다 오래된 마커는 타임라인에 나올 일이 없다.
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 24LL * 3600 * 1000;
    timelineEvents_.erase(
        std::remove_if(timelineEvents_.begin(), timelineEvents_.end(),
                       [cutoff](const TimelineEvent& e) { return e.atMs < cutoff; }),
        timelineEvents_.end());
    refreshTimeline();
}

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
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kCritical)));

    // 2. 비상 로그 조회 탭에 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, roiId, qint64(timestampMs),
                        QStringLiteral("FALL"), QStringLiteral("CAMERA"));
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
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kHigh)));

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, roiId, qint64(timestampMs),
                        QStringLiteral("EGRESS"), QStringLiteral("CAMERA"));
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
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kWarn)));

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, kRoiIdNone, qint64(timestampMs),
                        QStringLiteral("VITAL_ABNORMAL"), QStringLiteral("WEARABLE"));
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
    if (roiId < 0) return QStringLiteral("%1 · 채널 %2").arg(currentRoomName()).arg(channel + 1);
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
    // 빈 방에는 침대가 없다. 막지 않으면 102호를 보는 중에 채널을 바꿀 때마다
    // 101호 침대 폴리곤이 설정 스테이지에 다시 그려진다.
    if (selectedRoom_ != 0) {
        if (channelViews[channel]) channelViews[channel]->setZones({});
        if (roiEditorView && roiEditChannel == channel) roiEditorView->setZones({});
        return;
    }

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
    // (이후는 onReadyRead가 프레임마다 갱신).
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

// ═══════════════════════════════════════════════════════════
//  원격 방송(인터콤)
// ═══════════════════════════════════════════════════════════
void MainWindow::onMicToggled(bool on)
{
    micButton->setText(on ? QStringLiteral("🔴 방송 중")
                          : QStringLiteral("🎤 방송"));
    micButton->setProperty("active", on);
    micButton->style()->unpolish(micButton);
    micButton->style()->polish(micButton);
    qDebug() << (on ? "인터콤 방송 시작" : "인터콤 방송 종료");
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


// 낙상/침상이탈이 하나라도 활성이면 경보 버튼을 빨강 채움으로, 아니면 차분한 아웃라인으로.
void MainWindow::refreshAlarmButton()
{
    if (!alarmClearButton) return;
    bool anyActive = false;
    for (int ch = 0; ch < 4; ++ch)
        if (fallActive[ch] || bedEgressActive[ch] || vitalAbnormalActive[ch]) { anyActive = true; break; }
    updateAlarmBanner();   // 경보 토스트 표시/문구 갱신 (활성 시에만 내려온다)

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
    auto* item = logTable->item(row, LogWhen);
    if (!item) return;

    // 우측 재생기 위에 "언제 · 무슨 일 · 누구 · 어디" 한 줄. 영상만 덩그러니
    // 띄우면 지금 보는 게 어느 사건인지 표에서 눈을 떼는 순간 잊는다.
    if (eventContextLabel) {
        const QString who = logTable->item(row, LogResident)
                                ? logTable->item(row, LogResident)->text() : QString();
        const QString what = logTable->item(row, LogType)
                                 ? logTable->item(row, LogType)->text() : QString();
        const QString place = logTable->item(row, LogPlace)
                                  ? logTable->item(row, LogPlace)->text() : QString();
        eventContextLabel->setText(
            QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
                .arg(item->text(), what, who, place));
        eventContextLabel->show();
    }

    const QString url = item->data(LogClipUrl).toString();

    // 이벤트↔NVR 연결용 — 이 행의 채널/시각을 기억해뒀다가 "NVR에서 이어보기"에서 쓴다.
    // 클립이 없어도(저장 실패·보관기간 경과) 연속녹화에는 남아 있을 수 있으므로
    // 이 값은 URL 유무와 무관하게 채운다.
    selectedEventChannel_ = item->data(LogChannel).toInt();
    selectedEventTimestampMs_ = item->data(LogTimestamp).toLongLong();
    if (nvrJumpButton)
        nvrJumpButton->setEnabled(selectedEventChannel_ >= 0 && selectedEventTimestampMs_ > 0);

    if (url.isEmpty()) {
        qDebug() << "블랙박스 재생 요청 — row" << row << "(클립 URL 없음)";
        if (blackboxPlaceholder)
            blackboxPlaceholder->setText(
                QStringLiteral("이 이벤트에는 저장된 클립이 없습니다.\n"
                               "아래 [이 시점 NVR에서 이어보기]를 눌러 보세요."));
        if (blackboxStack) blackboxStack->setCurrentIndex(0);
        return;
    }

    // 영상을 열었으므로 이 이벤트는 '확인' 처리
    markLogConfirmed(row);

    qDebug() << "블랙박스 재생 요청 —" << url;
    // 인라인 플레이어(이 페이지 우측)에서 바로 재생 — 페이지 이동 없음.
    playBlackboxClip(url);
}


// 상태 컬럼(3번)을 '미확인' → '확인'으로 바꾸고 초록색으로 표시.
void MainWindow::markLogConfirmed(int row)
{
    if (!logTable || row < 0 || row >= logTable->rowCount()) return;

    auto* statusItem = logTable->item(row, LogStatus);
    if (!statusItem) return;
    if (statusItem->text() == QStringLiteral("확인")) return;   // 이미 확인됨

    // 원장에도 남긴다 — 누가 언제 확인했는지는 사고 이후 되짚을 때 필요한 기록이다.
    // 화면에만 '확인'을 칠하면 앱을 껐다 켠 순간 그 사실이 사라진다.
    auto* whenItem = logTable->item(row, LogWhen);
    const qint64 eventId = whenItem ? whenItem->data(LogEventId).toLongLong() : -1;
    if (eventId > 0 && QSqlDatabase::database().isOpen()) {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "UPDATE events SET confirmed_at = NOW(), confirmed_by = ? "
            "WHERE event_id = ? AND confirmed_at IS NULL"));
        q.addBindValue(currentUser.name);
        q.addBindValue(eventId);
        if (!q.exec())
            qDebug() << "이벤트 확인 기록 실패:" << q.lastError().text();
    }
    // eventId < 0 = 방금 소켓으로 온 행. 서버가 같은 이벤트를 원장에 쓰는 중이라
    // 여기서 UPDATE할 대상이 아직 없다 — 다음 조회 때 DB 행으로 대체되며,
    // 그때 확인 표시는 풀린다. 화면 표시만 먼저 바꿔 둔다.

    // 텍스트 변경 중 자동 재정렬로 행이 움직여 엉뚱한 셀을 건드리는 것 방지
    const bool wasSorting = logTable->isSortingEnabled();
    logTable->setSortingEnabled(false);

    statusItem->setText(QStringLiteral("확인"));
    if (!currentUser.name.isEmpty())
        statusItem->setToolTip(QStringLiteral("%1 확인").arg(currentUser.name));

    logTable->setSortingEnabled(wasSorting);
    refreshEventLog();   // 상태 배지 색 + 요약(미확인 수) 갱신
}


// 로그가 바뀔 때마다 호출 — 이벤트/상태 셀을 색으로 구분한다.
//  · 이벤트: 낙상=빨강, 침상이탈=주황   · 상태: 미확인=빨강, 확인=초록
void MainWindow::refreshEventLog()
{
    if (!logTable) return;

    const QColor cCritical(QString::fromLatin1(kCritical));
    const QColor cHigh(QString::fromLatin1(kHigh));
    const QColor cWarn(QString::fromLatin1(kWarn));
    const QColor cNormal(QString::fromLatin1(kNormal));
    const QColor cSub(QString::fromLatin1(kTextSub));

    for (int r = 0; r < logTable->rowCount(); ++r) {
        auto* typeItem = logTable->item(r, LogType);
        auto* stItem = logTable->item(r, LogStatus);
        if (!typeItem || !stItem) continue;

        // 종류별 색은 관제 화면의 경보색·타임라인 마커색과 같은 토큰을 쓴다 —
        // 같은 사건이 화면마다 다른 색이면 색으로 종류를 외울 수 없다.
        const QString t = typeItem->text();
        typeItem->setForeground(t == QStringLiteral("낙상")     ? cCritical
                                : t == QStringLiteral("침상이탈") ? cHigh
                                                                   : cWarn);
        typeItem->setTextAlignment(Qt::AlignCenter);

        const bool confirmed = (stItem->text() == QStringLiteral("확인"));
        stItem->setForeground(confirmed ? cNormal : cCritical);
        stItem->setTextAlignment(Qt::AlignCenter);

        if (auto* dt = logTable->item(r, LogWhen)) dt->setForeground(cSub);
        if (auto* src = logTable->item(r, LogSource)) {
            src->setForeground(cSub);
            src->setTextAlignment(Qt::AlignCenter);
        }
        if (auto* res = logTable->item(r, LogResident))
            res->setTextAlignment(Qt::AlignCenter);
    }

    refreshLogSummary();
}



// 실시간으로 얹힌 행이 현재 조건에 맞는지 걸러 낸다(행을 지우지 않고 숨김).
// 조건 필터링의 본체는 SQL(reloadEventLog)이며 이 함수는 그 보조다.
void MainWindow::applyLogFilters(bool withDates)
{
    if (!logTable) return;

    // 조건 필터링의 본체는 SQL(reloadEventLog)이다. 이 함수는 그 뒤에 실시간으로
    // 얹힌 행이 현재 조건에 맞는지만 걸러 낸다 — 방금 도착한 다른 채널 이벤트가
    // 조건을 무시하고 표에 남아 있으면 관제사가 필터를 믿지 못하게 된다.
    const QString evtSel = filterEventType ? filterEventType->currentText() : QString();
    const bool evtAll = evtSel.isEmpty() || evtSel == QStringLiteral("전체 이벤트");

    const QString roomSel = filterRoom ? filterRoom->currentText() : QString();
    const bool roomAll = roomSel.isEmpty() || roomSel == QStringLiteral("전체 병실");

    const int chSel = filterChannel ? filterChannel->currentData().toInt() : -1;

    const QString confSel = filterConfirmed ? filterConfirmed->currentText() : QString();

    for (int row = 0; row < logTable->rowCount(); ++row) {
        bool show = true;

        if (!evtAll) {
            auto* evtItem = logTable->item(row, LogType);
            show = evtItem && evtItem->text() == evtSel;
        }

        // 병실 — 위치 문구("{room} · 채널 N[ · 침대 M]")가 선택된 room으로 시작하는지.
        // room이 하나뿐인 지금은 늘 참이지만, 카메라가 늘어 room이 여럿이 되면
        // 이 매칭이 바로 실제 필터로 동작한다(로직 변경 불필요).
        if (show && !roomAll) {
            auto* placeItem = logTable->item(row, LogPlace);
            show = placeItem && placeItem->text().startsWith(roomSel);
        }

        if (show && chSel >= 0) {
            auto* whenItem = logTable->item(row, LogWhen);
            show = whenItem && whenItem->data(LogChannel).toInt() == chSel;
        }

        if (show && !confSel.isEmpty() && confSel != QStringLiteral("전체")) {
            auto* st = logTable->item(row, LogStatus);
            const bool confirmed = st && st->text() == QStringLiteral("확인");
            show = (confSel == QStringLiteral("확인만")) ? confirmed : !confirmed;
        }

        // 날짜 범위 — 0열 "yyyy-MM-dd HH:mm:ss"의 앞 10글자만 파싱
        if (show && withDates && filterDateFrom && filterDateTo) {
            auto* dtItem = logTable->item(row, LogWhen);
            const QDate d = dtItem
                                ? QDate::fromString(dtItem->text().left(10), QStringLiteral("yyyy-MM-dd"))
                                : QDate();
            show = d.isValid() && d >= filterDateFrom->date() && d <= filterDateTo->date();
        }

        logTable->setRowHidden(row, !show);
    }

    refreshEventLog();   // 필터/삽입 후 행 색 갱신
}

void MainWindow::loadPatientsFromDb()
{
    const QString room = currentRoomName();
    for (int ch = 0; ch < 4; ++ch) {
        patients[ch] = { QStringLiteral("미배정"),
                         QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1),
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
        // 위치는 room+채널로 표기. 침상 구분이 생기기 전까지는 같은 채널이면 같은 값.
        info.bed     = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
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
        // 타일 좌상단 이름(LIVE 옆)도 같은 소스에서 만든다.
        if (channelViews[ch]) channelViews[ch]->setDisplayName(tileDisplayName(ch));
        // 침대 이름표·매핑 콤보도 새 입소자 구성으로 다시 그린다 — 이름을 고치거나
        // 퇴원시켰는데 침대 라벨만 옛 이름으로 남으면 관제사가 오판한다.
        refreshRoiZones(ch);
    }
    rebuildVitalCards();
    refreshResourceTree();
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
        "admitted_at, discharge_due, status, notes "
        "FROM residents WHERE resident_id = ?"));
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
    // value(2)=bed 는 침대 항목 제거로 폼에 반영하지 않는다.
    // DB는 0~3으로 저장, 화면엔 1~4로 보여준다(사람이 읽기 쉬운 채널 번호).
    editCameraId->setText(q.value(3).isNull() ? QString()
                                              : QString::number(q.value(3).toInt() + 1));
    editWearableId->setText(q.value(4).isNull() ? QString() : q.value(4).toString());
    setCombo(editRiskLevel, q.value(5).toString());
    if (q.value(6).toDate().isValid())  editAdmittedAt->setDate(q.value(6).toDate());
    if (q.value(7).toDate().isValid())  editDischargeDue->setDate(q.value(7).toDate());
    setCombo(editStatus, q.value(8).toString());
    editNotes->setPlainText(q.value(9).toString());

    refreshAdmissionTable(selectedResidentId);

    qDebug() << "입소자 선택 — ID:" << selectedResidentId;
}

void MainWindow::onNewResident()
{
    selectedResidentId = -1;
    editName->clear();
    editRoom->clear();
    editCameraId->clear();
    editWearableId->clear();
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
    m[QStringLiteral("호실")]        = editRoom->text().trimmed();
    m[QStringLiteral("카메라 채널")] = editCameraId->text().trimmed();
    m[QStringLiteral("웨어러블 ID")] = editWearableId->text().trimmed();
    m[QStringLiteral("위험도")]      = editRiskLevel->currentText();
    m[QStringLiteral("입원일")]      = editAdmittedAt->date().toString(Qt::ISODate);
    m[QStringLiteral("퇴원 예정일")] = editDischargeDue->date().toString(Qt::ISODate);
    m[QStringLiteral("상태")]        = editStatus->currentText();
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
            " discharge_due, status, notes) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    } else {
        q.prepare(QStringLiteral(
            "UPDATE residents SET name=?, room=?, bed=?, camera_id=?, "
            " wearable_id=?, risk_level=?, admitted_at=?, discharge_due=?, "
            " status=?, notes=? WHERE resident_id=?"));
    }

    q.addBindValue(editName->text().trimmed());
    // room/bed 컬럼은 NOT NULL이라 빈 칸도 빈 문자열로 채운다(침대는 UI에서 제거).
    // 주의: QString()은 SQL NULL로 들어가 NOT NULL 위반 → 반드시 non-null 빈 문자열.
    q.addBindValue(editRoom->text().trimmed());
    q.addBindValue(QStringLiteral(""));
    q.addBindValue(channelOrNull(editCameraId->text()));
    q.addBindValue(textOrNull(editWearableId->text()));
    q.addBindValue(editRiskLevel->currentText());
    q.addBindValue(editAdmittedAt->date());
    q.addBindValue(editDischargeDue->date());
    q.addBindValue(editStatus->currentText());
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