#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QByteArray>
#include <QLabel>
#include <QPolygonF>
#include <QTimer>
#include <QDate>
#include <QString>
#include <QLineEdit>
#include <QTextEdit>
#include <QSet>
#include <QMap>
#include <QPixmap>

#include <QHash>
#include <QVector>

#include "auth.h"
// MqttQtManager 와 WearableData / AlarmCommand.
//
// MqttQtManager 는 포인터로만 쓰니 전방 선언으로 충분해 보이지만, 아래 슬롯들이
// 그 구조체를 인자로 받는다. moc 은 슬롯 인자마다 QMetaTypeId 를 건드리는데,
// Q_DECLARE_METATYPE 이 그보다 늦게 보이면
//   specialization of 'QMetaTypeId<WearableData>' after instantiation
// 으로 컴파일이 깨진다. 선언이 먼저 오도록 통째로 include 한다.
#include "mqttqtmanager.h"
#include "clickslider.h"



// 🌟 명세서 3번에 정의된 16바이트 리틀엔디언 구조체 그대로 구현
// (서버 protocol/video_stream.h와 반드시 동일하게 유지할 것)
#pragma pack(push, 1)
struct dbj_vs_header_t {
    uint16_t magic;         // 2B (0xDB4B)
    uint8_t version;        // 1B (0x01)
    uint8_t channel;        // 1B (0~3)
    uint64_t timestamp_ms;  // 8B (Unix epoch 밀리초)
    uint32_t payload_len;   // 4B (JPEG 이미지 크기)
};

// 역방향(클라→서버) 제어 메시지 — 침대 ROI / 낙상 확인 전송용. magic=0xDB4C.
struct dbj_ctrl_header_t {
    uint16_t magic;         // 2B (0xDB4C)
    uint8_t version;        // 1B (0x01)
    uint8_t type;           // 1B (아래 kCtrl* 상수 참조)
    uint8_t channel;        // 1B (0~3)
    uint8_t point_count;    // 1B (이어지는 점 개수 / 위험도 값)
    uint16_t reserved;      // 2B (0, CAMERA_SET에선 URL 바이트 길이)
};
struct dbj_roi_point_t {
    uint16_t x;             // 정규화 x × 10000 (0~10000)
    uint16_t y;             // 정규화 y × 10000 (0~10000)
};
// 카메라 이미지 파라미터 (밝기/대비/채도) — IMAGE_SET 헤더 뒤 1개. 0~100.
struct dbj_image_params_t {
    uint8_t brightness;     // 0~100 (서버가 카메라 실제 범위로 매핑)
    uint8_t contrast;       // 0~100
    uint8_t saturation;     // 0~100
};                          // 3B
// 카메라 포커스 — FOCUS_SET 헤더 뒤 1개.
struct dbj_focus_t {
    uint8_t  mode;          // 0=전체 자동초점, 1=클릭 영역 초점
    uint8_t  reserved;      // 0
    uint16_t x;             // 클릭 중심 정규화 x × 10000
    uint16_t y;             // 정규화 y × 10000
};                          // 6B

// 역방향(서버→클라) 이벤트 알림 — 낙상. magic=0xDB4D. 페이로드 없이 헤더(18B)만.
// 영상 프레임(0xDB4B)과 같은 소켓(5500)으로 섞여 들어오며, magic으로 구분한다.
struct dbj_evt_header_t {
    uint16_t magic;         // 2B (0xDB4D)
    uint8_t  version;       // 1B (0x01)
    uint8_t  type;          // 1B (0x01 = 낙상 확정)
    uint8_t  channel;       // 1B (0~3, 발생 채널)
    uint8_t  reserved;      // 1B (0)
    uint16_t x;             // 2B (발생 위치 정규화 x ×10000)
    uint16_t y;             // 2B (발생 위치 정규화 y ×10000)
    uint64_t timestamp_ms;  // 8B (서버 Unix time ms)
};                          // 18B
#pragma pack(pop)

// 제어 메시지 상수 (서버와 합의된 값 — protocol/video_stream.h와 동일하게 유지)
static constexpr uint16_t kCtrlMagic = 0xDB4C;
static constexpr uint8_t kCtrlRoiSet = 0x01;
static constexpr uint8_t kCtrlRoiClear = 0x02;
static constexpr uint8_t kCtrlCameraSet = 0x05;    // 채널 카메라 연결 (헤더 뒤 RTSP URL)
static constexpr uint8_t kCtrlCameraClear = 0x06;  // 채널 카메라 해제
static constexpr uint8_t kCtrlImageSet = 0x07;     // 카메라 이미지 파라미터 (헤더 뒤 dbj_image_params_t)
static constexpr uint8_t kCtrlFocusSet = 0x08;     // 카메라 포커스 (헤더 뒤 dbj_focus_t)
static constexpr uint8_t kFocusWhole = 0;          // 전체 자동초점
static constexpr uint8_t kFocusArea = 1;           // 클릭 영역 초점
static constexpr int kRoiCoordScale = 10000;
static constexpr int kCameraUrlMax = 512;          // DBJ_CAMERA_URL_MAX

// 이벤트 메시지 상수 (서버 스펙)
static constexpr uint16_t kEvtMagic = 0xDB4D;
static constexpr uint8_t kEvtFall = 0x01;       // 낙상 확정
static constexpr uint8_t kEvtBedEgress = 0x02;  // 침대 이탈
static constexpr uint8_t kEvtVitalAbnormal = 0x03;  // 웨어러블 생체데이터 이상 (x,y 미사용)

class VideoView;     // 영상+ROI 오버레이 위젯 (videoview.h)
class FramePreview;  // 이미지 탭 적용 전/후 프리뷰 (videoview.h)
class Sparkline;  // 심박 미니 추세 그래프 (sparkline.h)
class QDialog;
class QPushButton;

class QTableWidget;
class QComboBox;
class QDateEdit;
class QSlider;
class QVBoxLayout;
class QStackedWidget;
class QMediaPlayer;
class QVideoWidget;
class QUdpSocket;
class QListWidget;
class QTextBrowser;
class QGridLayout;
class QResizeEvent;
class QPropertyAnimation;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// 채널(=병상)별 환자 / 웨어러블 정보 묶음
struct PatientInfo {
    QString name;   // 환자 이름
    QString bed;    // 위치 표기 — 병실/침대 제거 후 "채널 N"을 담는다(오버레이/바이탈 공용)
    QString room;   // 호실 — MQTT 알림 명령에 실어 보낸다(알림 노드가 LED에 띄운다)
};

// 입소자 한 명의 표시 정보. 한 채널에 여러 명이 있을 수 있어 채널 단위인
// PatientInfo(대표 1명)와 따로 둔다 — 바이탈은 사람마다 따로 와야 한다.
struct ResidentInfo {
    QString name;
    QString bed;        // 위치 표기("채널 N")
    QString room;       // 호실
    int     channel = -1;
};

// 입소자별 최신 웨어러블 값. MQTT 로 들어온 것만 담는다.
// 한 번도 안 들어온 사람은 received=false 로 남아 화면에 "--" 가 뜬다 —
// 값이 없는데 그럴듯한 숫자를 보여주면 관제사가 오판한다.
// (웨어러블 JSON에는 temperature 필드도 있지만 기기가 실제로 채워 보내지 않는다 →
//  화면에 띄우지 않고 보관도 하지 않는다. 값이 들어오기 시작하면 여기에 되살리면 된다.)
struct VitalSample {
    bool   received    = false;
    int    heartRate   = 0;
    int    spo2        = 0;
    qint64 arrivedAtMs = 0;   // 값이 오래됐는지 판단용
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const Auth::SessionUser& user, QWidget *parent = nullptr);
    ~MainWindow();

    // 창이 닫힌 이유가 "로그아웃"이면 true — main()이 로그인 창을 다시 띄운다.
    bool logoutRequested() const { return logoutRequested_; }

private slots:
    void onLogoutClicked();      // 로그아웃 — 확인 후 창을 닫고 로그인으로 복귀
    void onHelpClicked();        // 도움말 — 전체 기능 설명 창 열기

    void onReadyRead();          // 명세서 가이드라인 구현 슬롯 (영상 수신)
    void onSocketStateChanged(QAbstractSocket::SocketState state);
    void connectToServer();      // 영상 서버 접속/재접속
    void updateClock();          // 상단 실시간 시계
    void updateVitals();         // 웨어러블 바이탈 갱신(현재는 시뮬레이션)
    void updateCareTime();       // 케어 타임 대시보드 갱신(care_logs 재조회)
    void onRoiButtonClicked();   // "ROI 지정" — 채널 선택 후 그리기 시작
    void onRoiClearClicked();    // "ROI 제거" — 로컬 + 서버 판정 영역 삭제
    void onRoiVisibilityToggled(bool on);  // "ROI 표시" 토글
    void onRoiCompleted(int channel, const QPolygonF& normPts);  // 그리기 완료 → 전송
    void onMicPressed();    // 마이크 버튼 누름 — 방송 시작
    void onMicReleased();   // 마이크 버튼 뗌 — 방송 종료
    void onAlarmClearClicked();  // 경보 해제
    void onAddCameraClicked();   // "카메라 연결" — CCTV IP 입력 → 서버로 전송
    void onSearchCameraClicked();// "카메라 검색" — ONVIF WS-Discovery로 같은 망 카메라 탐색
    void onCameraClearClicked(); // "카메라 해제" — 모든 채널 CAMERA_CLEAR 전송

    // ── MQTT (웨어러블·알림 노드) ─────────────────────────
    // 영상 경로(TCP)와 별개로, 브로커를 통해 들어오는 것들을 받는 슬롯.
    void onWearableData(const WearableData& data);   // 생체·낙상 원본 도착
    void onMqttAlarm(const AlarmCommand& cmd);       // 알림 노드로 나간 명령을 엿들음
    void onMqttConnected();
    void onMqttDisconnected();
    void onMqttError(const QString& message);

    // TAB2: 이벤트 기록
    void onLogRowActivated(int row, int column);

    // TAB3: DB 관리
    void onResidentSearch();   // 이름으로 입소자 검색(재원·퇴원 전체)
    void onNewResident();
    void onSaveResident();
    void onDischargeResident();
    void onReadmitResident();                          // 퇴원자 → 새 입원 에피소드 생성
    void onAdmissionRowActivated(int row, int column);  // 이력 행 더블클릭 → 변경 내역 팝업

private:
    Ui::MainWindow *ui;
    // 2-Pi 분할: 채널을 두 라즈베리에 2+2로 나눠 받는다. 서버마다 소켓·수신버퍼 1쌍.
    //   ch0·ch1 → Pi A(sockets[0]) / ch2·ch3 → Pi B(sockets[1]).
    static constexpr int kNumServers = 2;
    static int serverForChannel(int ch) { return ch < 2 ? 0 : 1; }
    QTcpSocket *sockets[kNumServers] = {};
    QByteArray buffers[kNumServers];   // 연결마다 바이트 스트림이 별개 → 버퍼도 분리
    QTcpSocket* socketForChannel(int ch) { return sockets[serverForChannel(ch)]; }
    VideoView* channelViews[4] = {};  // 4분할 영상+ROI 오버레이 위젯
    QWidget* videoCards[4] = {};      // 영상 카드(스포트라이트 재배치용)
    QGridLayout* videoGrid = nullptr; // 영상 월 그리드(재배치 대상)
    int focusedChannel_ = -2;         // 스포트라이트 채널(-1=균등 2×2, -2=아직 미배치)
    // 낙상·침상이탈 감지 시 그 채널을 크게, 나머지는 작게. -1이면 균등 2×2로 복귀.
    void setVideoFocus(int channel);
    // 채널별 마지막 카메라 RTSP URL — Pi가 잠깐 끊겼다 붙을 때 자동 재전송용(세션 한정,
    // 비밀번호가 포함돼 QSettings엔 저장하지 않는다). 비어 있으면 미연결.
    QString lastCameraUrl_[4];
    // 채널별 카메라 연결 여부(QSettings 지속) — 서버는 Qt를 껐다 켜도 스트리밍을
    // 유지하므로, 재시작 후 URL이 없어도 이 플래그로 "해제" 대상을 안다. 비어 있으면 미연결.
    bool cameraActive_[4] = {};
    bool serverConnected_[kNumServers] = {};  // Pi별 직전 연결 상태(재접속 전이 감지)
    bool videoSuppressed_[4] = {};   // 해제한 채널 — 재연결 전까지 들어오는 프레임 무시(검은 화면 유지)
    bool roiDrawing = false;     // 현재 어느 채널이든 ROI 그리는 중인지
    bool fallActive[4] = {};     // 채널별 낙상 경보 활성 상태
    bool bedEgressActive[4] = {};  // 채널별 침상이탈 경보 활성 상태
    bool vitalAbnormalActive[4] = {};  // 채널별 생체신호 이상 경보 활성 상태

    // ── 대시보드 UI 구성 요소 ──────────────────────────────
    PatientInfo patients[4];     // 병상별 환자 정보
    QLabel* clockLabel = nullptr;      // 상단 실시간 시계
    QPushButton* themeToggleButton = nullptr;  // 라이트/다크 테마 토글
    QPushButton* helpButton = nullptr;         // ⓘ 도움말
    QDialog* helpDialog = nullptr;             // 기능 설명 창(1회 생성 후 재사용)
    QListWidget* helpList = nullptr;           // 좌측 주제 목록
    QTextBrowser* helpBrowser = nullptr;       // 우측 내용
    void renderHelpTopic(int idx);             // 선택 주제를 현재 테마 색으로 렌더

    // ── 로그인 세션 ──
    Auth::SessionUser currentUser;         // 현재 로그인한 사용자
    bool logoutRequested_ = false;         // 종료 vs 로그아웃 구분
    QLabel* userNameLabel = nullptr;       // 헤더의 사용자 이름
    QLabel* userAvatarLabel = nullptr;     // 이름 첫 글자 원형 배지
    QPushButton* logoutButton = nullptr;   // 로그아웃 버튼

    bool darkMode = true;              // 현재 다크모드 여부 (기본 다크 관제 톤)
    void toggleTheme();                // 테마 전환 + 재적용
    QLabel* statusDot = nullptr;       // 서버 연결 상태 표시등
    QLabel* statusText = nullptr;      // 서버 연결 상태 문구
    // ── 바이탈 카드 위젯들 ─────────────────────────────────
    // 키는 카드 하나를 가리키는 식별자다: 입소자가 있으면 resident_id(양수),
    // 아무도 배정되지 않은 채널이면 -(채널+1). 음수 키는 vitals_ 에 값이 없어
    // 자연히 "대기" 상태로 표시된다.
    // ★ 카드 개수가 입소자 수에 따라 변하므로 고정 배열이 아니라 해시다.
    //   rebuildVitalCards() 가 통째로 비우고 다시 채운다.
    QHash<int, QLabel*> spo2Values;         // 산소포화도 값
    QHash<int, QLabel*> hrValues;           // 심박수 값
    QHash<int, QLabel*> vitalStatusDots;    // 바이탈 상태등
    QHash<int, QLabel*> vitalStatusBadges;  // 상태 배지(정상/주의/위험)
    QHash<int, QLabel*> vitalNameLabels;    // 입소자 이름

    // ── MQTT ─────────────────────────────────────────────
    MqttQtManager* mqtt = nullptr;     // 브로커 연결(웨어러블 수신 + 알림 노드 제어)
    // 웨어러블 기기 id → 입소자(resident_id). residents 의 wearable_id 로 만든다.
    // ★ 채널이 아니라 사람으로 잇는다 — 한 채널에 여러 명이면 채널로는 누구 값인지
    //   구분이 안 돼 나중에 온 값이 앞 값을 덮어쓴다. 기기 id 는 사람마다 고유하다.
    QHash<QString, int> wearableToResident;
    QHash<int, ResidentInfo> residentInfo_;   // resident_id → 표시 정보
    QVector<int> residentsByChannel_[4];      // 채널 → 입소자 id 목록(표시 순서)
    QHash<int, VitalSample> vitals_;          // resident_id → 최신 생체값
    // 입소자별 심박 이력. ★ Sparkline 위젯 안에도 값이 쌓이지만, 카드를 다시 만들면
    //   위젯과 함께 사라진다. 입소자가 추가·퇴원할 때마다 남의 그래프까지 초기화되지
    //   않도록 이력은 위젯 밖인 여기에 두고, 카드를 만들 때 다시 부어넣는다.
    QHash<int, QVector<double>> hrHistory_;
    QHash<int, QLabel*> vitalBedLabels;       // 위치 표기("채널 N")
    QHash<int, Sparkline*> hrSpark;           // 심박 미니 추세 그래프
    QVBoxLayout* vitalListLayout_ = nullptr;  // 바이탈 카드 목록(재생성 대상)

    QTimer clockTimer;
    QTimer vitalsTimer;
    QTimer careTimeTimer;        // 케어 타임 대시보드 주기 갱신(care_logs 재조회)
    QTimer reconnectTimer;       // 영상 서버 자동 재접속

    // ── TAB 구조 ──────────────────────────────────────────
    // ── 좌측 네비 레일 + 본문 스택 (예전 상단 QTabWidget 대체) ──
    static constexpr int kNavCount = 5;
    QWidget* buildNavRail();          // 레일 구성(아이콘+라벨 5개 + 접기 토글)
    void     refreshNavIcons();       // 팔레트 전환 시 아이콘 색 재생성
    void     setNavCollapsed(bool on);// 접기/펼치기 (QSettings에 저장)
    QFrame*         navRail = nullptr;
    QPushButton*    navToggle = nullptr;
    QPushButton*    navBtns[kNavCount] = {};
    bool            navCollapsed_ = false;
    QStackedWidget* contentStack = nullptr;

    // TAB2: 이벤트 기록 (요약 카드 + 로그 표 + 인라인 블랙박스)
    QDateEdit* filterDateFrom = nullptr;
    QDateEdit* filterDateTo = nullptr;
    QComboBox* filterRoom = nullptr;
    QComboBox* filterEventType = nullptr;
    QTableWidget* logTable = nullptr;
    QLabel* blackboxPlaceholder = nullptr;
    QVideoWidget* blackboxVideoWidget = nullptr;
    QStackedWidget* blackboxStack = nullptr;
    QSlider* blackboxSeek = nullptr;
    QPushButton* blackboxPlayPauseButton = nullptr;
    QLabel* blackboxTimeLabel = nullptr;
    QMediaPlayer* blackboxPlayer = nullptr;
    bool blackboxSeeking = false;   // 사용자가 재생바를 잡고 있는 중
    QString blackboxUrl;            // 현재 재생/재시도 중인 클립 URL
    int blackboxRetries = 0;        // 저장 완료 전 재시도 횟수
    // 케어 타임 카드(채널당 1개) — 매 갱신마다 라벨 텍스트만 바꾼다(카드는 1회 생성).
    QLabel* careNameLabels[4] = {};     // 환자 이름
    QLabel* careMetaLabels[4] = {};     // "채널 N · 위치"
    QLabel* careBigLabels[4] = {};      // 오늘 케어시간 큰 값 ("45분"/"30초")
    QLabel* careSessionLabels[4] = {};  // "N회"
    QLabel* careLastLabels[4] = {};     // "HH:mm" (최근 케어)
    QWidget* buildCareTimeCard(int channel);

    // ── TAB3: DB 관리 ──────────────────────────────────────
    // 입소자 목록 = 카드 그리드(사람당 카드 1개). 카드 클릭 → 편집 다이얼로그.
    // ── 마스터(좌측 목록) ──
    QWidget*   residentCardHost = nullptr;   // 목록 행이 붙는 컨테이너(세로 리스트)
    QLabel*    residentCountLabel = nullptr; // "재원 N명 / 검색 결과 N명"
    QLineEdit* residentSearchEdit = nullptr; // 이름 검색창
    // 재원/전체/퇴원 필터 — 세그먼트 버튼. residentFilter_ 가 현재 모드.
    QString      residentFilter_ = QStringLiteral("재원");
    QPushButton* residentFilterBtns[3] = {}; // [0]재원 [1]전체 [2]퇴원
    int          selectedResidentCardId = -1; // 목록에서 강조 표시 중인 id
    // 상단 요약 통계 값
    QLabel* resSumActive = nullptr;   // 재원 N명
    QLabel* resSumHigh   = nullptr;   // 위험 상
    QLabel* resSumMid    = nullptr;   // 위험 중
    QLabel* resSumLow    = nullptr;   // 위험 하
    QLabel* resSumCam    = nullptr;   // 채널 배정 N/4

    // ── 디테일(우측 인라인 편집) — 예전엔 팝업이었으나 페이지에 내장 ──
    QStackedWidget* residentDetailStack = nullptr;  // 0=플레이스홀더 / 1=편집기
    QLabel*  dlgAvatar        = nullptr;  // 이름 이니셜 원형 배지
    QLabel*  dlgNameBig       = nullptr;  // 큰 이름
    QLabel*  dlgSubMeta       = nullptr;  // "201호-2 · 채널 2" 등
    QLabel*  dlgStatusBadge   = nullptr;  // 재원/퇴원
    QLabel*  dlgRiskBadge     = nullptr;  // 위험도 상/중/하
    QPushButton* dlgDischargeBtn = nullptr;  // 상태에 따라 퇴원↔재입원 토글

    // 입원 이력 (admissions = 입원 에피소드, 입원할 때마다 1행)
    //   행 = 한 번의 입원, 더블클릭 → 그 기간의 변경 내역 팝업
    QTableWidget* admissionTable = nullptr;
    QLabel* admissionInfo = nullptr;     // "입원 N건" 안내 문구
    QWidget* admissionBox = nullptr;   // 입원 이력 패널(신규 등록 시 숨김)


    // 상세/편집 폼 — 기본정보 (병실/침대는 제거, 위치는 카메라 채널로 표기)
    QLineEdit* editName       = nullptr;
    QLineEdit* editCameraId   = nullptr;
    QLineEdit* editWearableId = nullptr;

    // 상세/편집 폼 — 케어 정보
    QComboBox* editCaregiver     = nullptr;  // caregivers 테이블에서 로드
    QComboBox* editRiskLevel     = nullptr;  // 상/중/하
    QDateEdit* editAdmittedAt    = nullptr;  // 입원일
    QDateEdit* editDischargeDue  = nullptr;  // 퇴원 예정일
    QComboBox* editStatus        = nullptr;  // 재원/퇴원

    // 상세/편집 폼 — 보호자 정보
    QLineEdit* editGuardianName     = nullptr;
    QLineEdit* editGuardianPhone    = nullptr;
    QLineEdit* editGuardianRelation = nullptr;

    // 특이사항
    QTextEdit* editNotes = nullptr;

    // 현재 선택된 resident_id (-1이면 신규 등록 모드)
    int selectedResidentId = -1;

    // DB 연결 상태 표시
    QLabel* dbStatusDot = nullptr;
    QLabel* dbStatusText = nullptr;

    // ── UI 빌드 헬퍼 ──────────────────────────────────────
    void buildUi();
    QWidget* buildHeader();
    QWidget* buildVideoWall();
    QWidget* buildVitalsPanel();
    QWidget* buildVideoCard(int channel);
    // 바이탈 카드 1장. key 는 입소자면 resident_id(양수), 미배정 채널이면 -(채널+1).
    QWidget* buildVitalCard(int key, const QString& name, const QString& bedText);
    // 바이탈 카드 목록을 입소자 구성에 맞춰 통째로 다시 만든다.
    // 입소자가 늘거나 줄면 카드 개수 자체가 달라져서 글자만 갈아끼울 수 없다.
    void rebuildVitalCards();
    void applyTheme();
    void setConnectionState(bool connected, const QString& text);
    // 경보 해제 버튼 강조 갱신 — 낙상/침상이탈이 하나라도 활성이면 빨강 채움, 아니면 차분한 아웃라인.
    void refreshAlarmButton();
    // 경보 중 창 전체 테두리 빨강 펄스 오버레이. (QWidget*로 보관 — 실체는 파일 로컬 클래스)
    QWidget* alarmOverlay_ = nullptr;
    void resizeEvent(QResizeEvent* e) override;   // 오버레이를 중앙 위젯 크기에 맞춤

    // 경보 토스트 — 감지 시 화면 상단에서 아래로 슬라이드해 내려오는 알림 카드.
    QFrame* alarmBanner_ = nullptr;
    QLabel* alarmSummaryLabel_ = nullptr;   // "채널 2에서 낙상 발생!"
    QPropertyAnimation* alarmAnim_ = nullptr;
    bool alarmToastShown_ = false;
    QWidget* buildAlarmBanner();            // 토스트 카드 생성(오버레이)
    void updateAlarmBanner();               // 활성 경보에 따라 문구·표시 갱신
    void animateAlarmToast(bool show);      // 위→아래 슬라이드 인/아웃

    // TAB2 빌드 헬퍼 (이벤트 기록)
    QWidget* buildEventLogTab();       // 필터 + 로그 표 + 인라인 블랙박스
    QWidget* buildSearchFilters();
    QWidget* buildLogTable();
    QWidget* buildBlackboxPlayer();    // 인라인 재생 카드(페이지 우측)
    void playBlackboxClip(const QString& url);   // 블랙박스 클립 재생
    void markLogConfirmed(int row);                // 영상 확인 → 상태 '확인'(초록) 마킹
    void applyLogFilters(bool withDates = false);  // 로그 표 필터링(이벤트/날짜)
    // 로그가 바뀔 때마다 행 색(이벤트/상태 배지)을 다시 칠하고 요약 카드 값을 갱신한다.
    void refreshEventLog();

    // 케어 타임은 이벤트 기록에서 분리해 자체 탭(채널별 카드 그리드)으로 뺀다.
    QWidget* buildCareTimeTab();


    // TAB3 빌드 헬퍼 (입소자 관리 — 마스터-디테일)
    QWidget* buildDbTab();
    QWidget* buildResidentSummary();    // 상단 요약 통계 바
    QWidget* buildResidentList();       // 좌측: 필터 탭 + 검색 + 신규 + 목록
    QWidget* buildResidentDetail();     // 우측: 플레이스홀더/편집기 스택
    QWidget* buildResidentFormBody();   // 편집기에 들어갈 폼(그룹들)만
    QWidget* buildAdmissionHistory();   // 입원 이력 표 + 안내 문구
    void setResidentFilter(const QString& mode);  // 재원/전체/퇴원 전환
    void openResidentEditor(int residentId);  // id<0이면 신규, 아니면 로드 후 우측에 표시
    void loadResidentIntoForm(int residentId);// residents → 폼 필드 채우기
    void refreshResidentDialogHeader();       // 편집기 상단 아바타/배지 갱신
    void refreshResidentSummary();            // 상단 요약 통계 재계산

    // TAB3 데이터 갱신
    // nameFilter 비어 있으면 현재 필터(재원/전체/퇴원), 있으면 이름 LIKE 검색
    void refreshResidentCards(const QString& nameFilter = QString());
    void refreshAdmissionTable(int residentId);   // residentId < 0 이면 표를 비운다
    // residents(status='재원')를 camera_id로 채널에 매핑해 patients[]를 DB로 채운다.
    void loadPatientsFromDb();
    // patients[]를 영상 오버레이·바이탈 카드 라벨에 다시 반영(등록/수정/퇴원 후 호출).
    void refreshPatientLabels();
    void showChangeLogDialog(int admissionId);    // 그 입원 건의 변경 내역 팝업

    // ── 변경 로그(resident_changes) 기록 ──
    // snapshotResident와 formSnapshot은 같은 "라벨 → 값" 체계를 쓴다(키가 같아야 비교 성립).
    QMap<QString, QString> snapshotResident(int id);   // UPDATE 전 DB 값 = 수정 전
    QMap<QString, QString> formSnapshot() const;       // 폼 입력 값 = 수정 후
    int  currentAdmissionId(int residentId);           // 가장 최근 입원 에피소드 id
    void logChanges(int residentId, int admissionId,
                    const QMap<QString, QString>& before,
                    const QMap<QString, QString>& after,
                    const QString& changeType);        // 달라진 필드만 INSERT

    // 낙상 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleFallEvent(int channel, quint64 timestampMs, float nx, float ny);

    // 침상 이탈 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleBedEgressEvent(int channel, quint64 timestampMs);
    void handleVitalAbnormalEvent(int channel, quint64 timestampMs);

    // ROI 다각형(정규화 0~1)을 서버로 전송. clear=true면 삭제 메시지.
    void sendRoi(int channel, const QPolygonF& normPts, bool clear = false);

    // 카메라 연결/해제를 서버로 전송 (CAMERA_SET/CLEAR).
    // sendCamera는 해당 채널 담당 Pi로 RTSP URL을 보낸다. 성공 시 true.
    bool sendCamera(int channel, const QString& rtspUrl);
    void sendCameraClear(int channel);
    // Pi 재접속 시, 그 Pi 담당 채널의 마지막 카메라 URL을 자동 재전송한다.
    void resendCamerasForServer(int serverIdx);
    // 어느 채널에 카메라가 붙어 있는지를 QSettings에 비트마스크로 남기고 복원한다.
    // URL(비밀번호 포함)은 저장하지 않되, 채널 번호만 있으면 해제(CAMERA_CLEAR)는
    // 가능하므로 Qt 재시작 후에도 "해제"가 동작하도록 활성 채널만 보존한다.
    void persistCameraActive();
    void restoreCameraActive();
    // 단일 CCTV IP → 채널별 RTSP URL 생성 (PNM-C16083RVQ 4센서 규약).
    static QString buildRtspUrl(const QString& ip, const QString& user,
                                const QString& password, int port,
                                const QString& profile, int channel);
    // 주어진 접속 정보로 4채널 카메라를 연결(수동 입력/검색 두 경로가 공유).
    // 4채널 URL 생성 → 담당 Pi로 전송 → 상태 반영 → QSettings 저장(비번 제외).
    void connectCameraWith(const QString& ip, const QString& user,
                           const QString& password, int port,
                           const QString& profile);

    QPushButton* roiButton = nullptr;   // "ROI 지정" 버튼
    QPushButton* roiClearButton = nullptr;   // "ROI 제거" 버튼
    QPushButton* roiToggleButton = nullptr;  // "ROI 표시" 토글
    QPushButton* micButton = nullptr;        // 🎤 원격 방송(인터콤) — press-and-hold
    QPushButton* alarmClearButton = nullptr; // 경보 해제 (현장 사이렌/LED 끄기)
    QPushButton* addCameraButton = nullptr;  // 📷 카메라 연결 (CCTV IP 입력→서버 전송)
    QPushButton* searchCameraButton = nullptr; // 🔍 카메라 검색 (ONVIF WS-Discovery)
    QPushButton* clearCameraButton = nullptr;  // 카메라 해제 (모든 채널 CAMERA_CLEAR)

    // "카메라 설정" — 상단 정식 탭. 좌측 컨트롤(연결/ROI/이미지) + 우측 라이브 영상의
    // 마스터-디테일 구성. 상단 공용 채널 레일(CH1~4)이 좌우 모두를 하나의 채널로 묶는다.
    QWidget* cameraSettingsTab_ = nullptr;     // 상단 탭 본문(가시성 판단용 멤버)
    QWidget* buildCameraSettingsTab();         // 탭 본문 구성(1회)
    // 카메라 설정 탭이 현재 활성 탭인지 — ROI/이미지 실시간 프리뷰 갱신 여부 판단.
    bool cameraSettingsVisible() const;

    // ── 카메라 설정 통합 UI ──
    // 3단 구성: [채널 스트립(썸네일)] │ [스테이지(큰 영상)] │ [인스펙터(모드별 설정)]
    QWidget* buildCamModeSegment();   // 상단 페이지 모드 세그먼트(연결/ROI/이미지)
    QWidget* buildCamChannelStrip();  // 좌측 채널 썸네일 타일 4개
    QWidget* buildCamInspector();     // 우측 인스펙터(헤더 + 모드별 페이지 스택)
    QWidget* buildCamConnectPage();   // 인스펙터 '연결' 페이지(접속 폼 + 검색표)
    QWidget* buildCamRoiPage();       // 인스펙터 'ROI' 페이지(안내 + 지정/제거/표시)
    QWidget* buildCamImagePage();     // 인스펙터 '이미지' 페이지(밝기/대비/채도 + 초점)
    QWidget* buildCamStagePanel();    // 가운데 라이브 영상/프리뷰 스테이지
    void selectCamChannel(int ch);    // 공용 채널 전환(스트립·스테이지·인스펙터 동기화)
    void setCamMode(const QString& mode);   // 연결/ROI/이미지 모드 전환
    void refreshCamChannelStatus();   // 채널 타일 배지 + 인스펙터 헤더 갱신
    QPushButton* camChannelBtns[4] = {};    // 채널 타일(썸네일을 품은 체크 버튼)
    QLabel*      camChannelStatus[4] = {};  // 타일 안 상태 배지
    FramePreview* camThumbs[4] = {};        // 타일 안 라이브 썸네일
    QLabel* camInspCh = nullptr;            // 인스펙터 헤더 "CH 2"
    QLabel* camInspPill = nullptr;          // 인스펙터 헤더 연결 상태 알약
    QLabel* camInspIp = nullptr;            // 인스펙터 헤더 카메라 주소
    QPushButton* camModeBtns[3] = {};       // [0]연결 [1]ROI [2]이미지 세그먼트
    QStackedWidget* camControlStack = nullptr;  // 인스펙터 페이지 스택
    QStackedWidget* camStageStack = nullptr;    // 스테이지(0=영상 / 1=이미지 프리뷰)
    QString camMode_ = QStringLiteral("연결");

    // ── 카메라 이미지 조절 (밝기/대비/채도) ──────────────────────
    // 슬라이더 값을 IMAGE_SET 제어 메시지로 서버에 보내면, 서버가 ONVIF Imaging으로
    // 실제 카메라에 적용한다. 대상 채널은 공용 채널(roiEditChannel)을 따른다.
    void     sendImageParams(int channel, int b, int c, int s);
    // 카메라 초점 — area=false 전체 자동초점, true 클릭 지점(nx,ny 정규화 0~1) 영역 초점.
    void     sendFocus(int channel, bool area, float nx, float ny);
    // imgAfter(실시간 프리뷰) 클릭 → 그 지점 영역 초점 전송.
    bool     eventFilter(QObject* obj, QEvent* ev) override;
    ClickSlider* imgBright = nullptr;
    ClickSlider* imgContrast = nullptr;
    ClickSlider* imgSaturation = nullptr;
    FramePreview* imgBefore = nullptr;                 // 적용 전 스냅샷
    FramePreview* imgAfter = nullptr;                   // 실시간(적용 후)
    QPixmap  lastFramePix_[4];                         // 채널별 최신 프레임(프리뷰용)

    // ── 카메라 탭(인라인) 위젯 ──
    QLineEdit* camIpEdit = nullptr;
    QLineEdit* camUserEdit = nullptr;
    QLineEdit* camPwEdit = nullptr;
    QTableWidget* discoveryTable = nullptr;    // 검색 결과(모델/IP/MAC)
    void syncDiscoveryTableHeight();           // 결과 표를 행 수만큼만(0행이면 숨김)
    QLabel* discoveryStatus = nullptr;
    QUdpSocket* discoverySocket = nullptr;     // 팝업 수명 동안 재사용
    QSet<QString> discoverySeen;               // 중복 응답 제거

    // ── ROI 탭(인라인 편집기) 위젯 ──
    VideoView* roiEditorView = nullptr;        // 선택 채널 영상을 팝업에 표시 + ROI 그림
    int roiEditChannel = 0;                    // 현재 편집 중인 채널(0~3)
    QPushButton* roiChannelButtons[4] = {};    // 채널 선택 버튼(1~4)
    QLabel* roiEditInfo = nullptr;
    void selectRoiChannel(int ch);             // 편집 채널 전환 → 영상/ROI 로드
};

#endif // MAINWINDOW_H
