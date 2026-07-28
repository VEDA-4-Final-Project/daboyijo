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

// 제어 메시지 상수 (서버와 합의된 값)
static constexpr uint16_t kCtrlMagic = 0xDB4C;
static constexpr uint8_t kCtrlRoiSet = 0x01;
static constexpr uint8_t kCtrlRoiClear = 0x02;
static constexpr int kRoiCoordScale = 10000;

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
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
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


    // TAB2: 비상 로그 조회 및 블랙박스
    void onSearchClicked();
    void onLogRowActivated(int row, int column);


    // TAB3: DB 관리
    void onResidentSelected(int row, int column);
    void onNewResident();
    void onSaveResident();
    void onDischargeResident();
    void onResidentSearch();



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
    bool roiDrawing = false;     // 현재 어느 채널이든 ROI 그리는 중인지
    bool fallActive[4] = {};     // 채널별 낙상 경보 활성 상태
    bool bedEgressActive[4] = {};  // 채널별 침상이탈 경보 활성 상태

    // ── 대시보드 UI 구성 요소 ──────────────────────────────
    PatientInfo patients[4];     // 병상별 환자 정보
    QLabel* clockLabel = nullptr;      // 상단 실시간 시계
    QPushButton* themeToggleButton = nullptr;  // 라이트/다크 테마 토글
    bool darkMode = false;             // 현재 다크모드 여부
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
    QLineEdit* residentSearchEdit = nullptr;

    // 상세/편집 폼 — 기본정보
    QLineEdit* editName       = nullptr;
    QDateEdit* editBirthDate  = nullptr;
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
    void refreshResidentTable(const QString& nameFilter=QString());
    void refreshCaregiverTable();

    // 낙상 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleFallEvent(int channel, quint64 timestampMs);

    // 침상 이탈 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleBedEgressEvent(int channel, quint64 timestampMs);

    // 로그 한 행을 '확인' 상태로 표시 (블랙박스 영상 열람 시)
    void markLogConfirmed(int row);

    // ROI 다각형(정규화 0~1)을 서버로 전송. clear=true면 삭제 메시지.
    void sendRoi(int channel, const QPolygonF& normPts, bool clear = false);

    QPushButton* roiButton = nullptr;   // "ROI 지정" 버튼
    QPushButton* roiClearButton = nullptr;   // "ROI 제거" 버튼
    QPushButton* roiToggleButton = nullptr;  // "ROI 표시" 토글
    QPushButton* micButton = nullptr;        // 🎤 원격 방송(인터콤) — press-and-hold
    QPushButton* alarmClearButton = nullptr; // 경보 해제 (현장 사이렌/LED 끄기)
};

#endif // MAINWINDOW_H
