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

#include "auth.h"



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
    uint8_t type;           // 1B (1=ROI_SET, 2=ROI_CLEAR, 3=FALL_CONFIRM)
    uint8_t channel;        // 1B (0~3)
    uint8_t point_count;    // 1B (이어지는 점 개수)
    uint16_t reserved;      // 2B (0)
};
struct dbj_roi_point_t {
    uint16_t x;             // 정규화 x × 10000 (0~10000)
    uint16_t y;             // 정규화 y × 10000 (0~10000)
};

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
static constexpr int kRoiCoordScale = 10000;
static constexpr int kCameraUrlMax = 512;          // DBJ_CAMERA_URL_MAX

// 이벤트 메시지 상수 (서버 스펙)
static constexpr uint16_t kEvtMagic = 0xDB4D;
static constexpr uint8_t kEvtFall = 0x01;       // 낙상 확정
static constexpr uint8_t kEvtBedEgress = 0x02;  // 침대 이탈

class VideoView;  // 영상+ROI 오버레이 위젯 (videoview.h)
class QDialog;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QComboBox;
class QDateEdit;
class QSlider;
class QVBoxLayout;
class QStackedWidget;
class QMediaPlayer;
class QVideoWidget;
class QUdpSocket;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// 채널(=병상)별 환자 / 웨어러블 정보 묶음
struct PatientInfo {
    QString name;   // 환자 이름
    QString bed;    // 병상 표기 (예: 201호-1)
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

    void onReadyRead();          // 명세서 가이드라인 구현 슬롯 (영상 수신)
    void onSocketStateChanged(QAbstractSocket::SocketState state);
    void connectToServer();      // 영상 서버 접속/재접속
    void updateClock();          // 상단 실시간 시계
    void updateVitals();         // 웨어러블 바이탈 갱신(현재는 시뮬레이션)
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
    void onSettingsClicked();    // "카메라 설정" — 탭 팝업(카메라/ROI) 열기


    // TAB2: 비상 로그 조회 및 블랙박스
    void onSearchClicked();
    void onLogRowActivated(int row, int column);


    // TAB3: DB 관리
    void onResidentSelected(int row, int column);
    void onNewResident();
    void onSaveResident();
    void onDischargeResident();



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
    // 채널별 마지막 카메라 RTSP URL — Pi가 잠깐 끊겼다 붙을 때 자동 재전송용(세션 한정,
    // 비밀번호가 포함돼 QSettings엔 저장하지 않는다). 비어 있으면 미연결.
    QString lastCameraUrl_[4];
    bool serverConnected_[kNumServers] = {};  // Pi별 직전 연결 상태(재접속 전이 감지)
    bool roiDrawing = false;     // 현재 어느 채널이든 ROI 그리는 중인지
    bool fallActive[4] = {};     // 채널별 낙상 경보 활성 상태
    bool bedEgressActive[4] = {};  // 채널별 침상이탈 경보 활성 상태

    // ── 대시보드 UI 구성 요소 ──────────────────────────────
    PatientInfo patients[4];     // 병상별 환자 정보
    QLabel* clockLabel = nullptr;      // 상단 실시간 시계
    QPushButton* themeToggleButton = nullptr;  // 라이트/다크 테마 토글

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
    QLabel* liveDots[4];               // 채널별 LIVE 표시등
    QLabel* tempValues[4];             // 채널별 체온 값
    QLabel* hrValues[4];               // 채널별 심박수 값
    QLabel* vitalStatusDots[4];        // 채널별 바이탈 상태등
    QLabel* vitalStatusBadges[4];      // 채널별 상태 배지(정상/주의/위험)
    QLabel* vitalUpdated[4];           // 채널별 마지막 갱신 시각

    QTimer clockTimer;
    QTimer vitalsTimer;
    QTimer reconnectTimer;       // 영상 서버 자동 재접속

    // ── TAB 구조 ──────────────────────────────────────────
    QTabWidget* tabWidget = nullptr;

    // TAB2: 비상 로그 조회 및 블랙박스
    QDateEdit* filterDateFrom = nullptr;
    QDateEdit* filterDateTo = nullptr;
    QComboBox* filterRoom = nullptr;
    QComboBox* filterEventType = nullptr;
    QTableWidget* logTable = nullptr;
    QDialog* blackboxDialog = nullptr;      // 블랙박스 재생 팝업(로그 더블클릭 시)
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
    QVBoxLayout* careTimeList = nullptr;

    // ── TAB3: DB 관리 ──────────────────────────────────────
    // 입소자 목록
    QTableWidget* residentTable = nullptr;

    // 상세/편집 폼 — 기본정보
    QLineEdit* editName       = nullptr;
    QLineEdit* editRoom       = nullptr;
    QLineEdit* editBed        = nullptr;
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

    // 요양사 목록
    QTableWidget* caregiverTable = nullptr;

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
    QWidget* buildVitalCard(int channel);
    void applyTheme();
    void setConnectionState(bool connected, const QString& text);

    // TAB2 빌드 헬퍼
    QWidget* buildLogArchiveTab();
    QWidget* buildSearchFilters();
    QWidget* buildLogTable();
    QWidget* buildBlackboxPlayer();
    void buildBlackboxDialog();   // 블랙박스 재생 팝업 생성(1회)
    QWidget* buildCareTimeDashboard();
    void playBlackboxClip(const QString& url);   // 블랙박스 클립 재생

    // TAB3 빌드 헬퍼
    QWidget* buildDbTab();
    QWidget* buildResidentSection();
    QWidget* buildResidentForm();
    QWidget* buildCaregiverSection();

    // TAB3 데이터 갱신
    void refreshResidentTable();
    void refreshCaregiverTable();

    // 낙상 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleFallEvent(int channel, quint64 timestampMs);

    // 침상 이탈 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleBedEgressEvent(int channel, quint64 timestampMs);

    // ROI 다각형(정규화 0~1)을 서버로 전송. clear=true면 삭제 메시지.
    void sendRoi(int channel, const QPolygonF& normPts, bool clear = false);

    // 카메라 연결/해제를 서버로 전송 (CAMERA_SET/CLEAR).
    // sendCamera는 해당 채널 담당 Pi로 RTSP URL을 보낸다. 성공 시 true.
    bool sendCamera(int channel, const QString& rtspUrl);
    void sendCameraClear(int channel);
    // Pi 재접속 시, 그 Pi 담당 채널의 마지막 카메라 URL을 자동 재전송한다.
    void resendCamerasForServer(int serverIdx);
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

    // "카메라 설정" 팝업 — 카메라·ROI 작업을 팝업 안에서 직접 수행(탭 전환).
    // 비모달로 띄워 ROI 그리기(영상 클릭)가 가능하게 한다. 1회만 생성(멤버 재사용).
    QPushButton* settingsButton = nullptr;     // ⚙️ 카메라 설정 (툴바)
    QDialog* cameraSettingsDialog = nullptr;
    void buildCameraSettingsDialog();          // 팝업 최초 1회 구성
    void startCameraDiscovery();               // 인라인 ONVIF 검색 → discoveryTable 채움

    // ── 카메라 탭(인라인) 위젯 ──
    QLineEdit* camIpEdit = nullptr;
    QLineEdit* camUserEdit = nullptr;
    QLineEdit* camPwEdit = nullptr;
    QLineEdit* camPortEdit = nullptr;
    QLineEdit* camProfileEdit = nullptr;
    QTableWidget* discoveryTable = nullptr;    // 검색 결과(모델/IP/MAC)
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
