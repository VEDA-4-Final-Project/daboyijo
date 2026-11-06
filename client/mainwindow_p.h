#ifndef MAINWINDOW_P_H
#define MAINWINDOW_P_H
// MainWindow 구현 파일들(mainwindow*.cpp)이 함께 쓰는 include·헬퍼·상수.
// 예전엔 9천 줄짜리 mainwindow.cpp 맨 위에 있던 것들이다 — 기능별로 파일을 나누면서
// 공용 부분만 이 내부 헤더로 옮겼다. (MainWindow 밖에서는 include하지 말 것)
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
#include <QRandomGenerator>
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
// 서버 인덱스(0=Pi A, 1=Pi B) → 저장된 호스트. 실제 정의는 StreamClient 하나뿐
// (QSettings 키·기본값이 두 곳에 갈라지지 않게).
QString serverHost(int idx) { return StreamClient::serverHost(idx); }
// 채널(0~3) → 담당 Pi의 호스트 (블랙박스 클립 URL 등 host가 필요한 곳용).
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

// (영상 스트림 CA 경로·인증서 CN은 StreamClient로 옮겼다)

// 이 시간이 지나도록 새 값이 안 오면 화면의 생체값을 "--" 로 되돌린다.
// 웨어러블이 빠졌거나 중계 노드가 죽은 걸 관제사가 알아야 하는데, 마지막 값이
// 계속 떠 있으면 멀쩡한 줄 안다.
constexpr qint64 kVitalStaleMs = 30000;   // 30초

// 입소자별로 보관하는 심박 이력 길이. Sparkline 위젯이 그리는 점 개수(capacity_)와
// 같게 맞춘다 — 카드를 다시 만들 때 이 이력을 그대로 부어넣어 추세를 복원한다.
constexpr int kHrHistoryMax = 40;
}  // namespace

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

// (JPEG 크기 상한·경보 해제 상수는 StreamClient / streamprotocol.h로 옮겼다)
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

#include "uiicons.h"

#endif // MAINWINDOW_P_H
