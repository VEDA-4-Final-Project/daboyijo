#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QByteArray>
#include <QLabel>
#include <QPolygonF>
#include <QTimer>
#include <QDate>
#include <QLineEdit>
#include <QTextEdit>
#include <QGroupBox>
#include <QFormLayout>
#include <QSplitter>



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

// 역방향(클라→서버) 제어 메시지 — 침대 ROI 전송용. magic=0xDB4C.
struct dbj_ctrl_header_t {
    uint16_t magic;         // 2B (0xDB4C)
    uint8_t version;        // 1B (0x01)
    uint8_t type;           // 1B (1=ROI_SET, 2=ROI_CLEAR)
    uint8_t channel;        // 1B (0~3)
    uint8_t point_count;    // 1B (이어지는 점 개수)
    uint16_t reserved;      // 2B (0)
};
struct dbj_roi_point_t {
    uint16_t x;             // 정규화 x × 10000 (0~10000)
    uint16_t y;             // 정규화 y × 10000 (0~10000)
};

// 서버→클라 이벤트 메시지 — 낙상 통보 등. magic=0xDB4D. (페이로드 없음)
struct dbj_evt_header_t {
    uint16_t magic;         // 2B (0xDB4D)
    uint8_t version;        // 1B (0x01)
    uint8_t type;           // 1B (1=낙상)
    uint8_t channel;        // 1B (0~3)
    uint8_t reserved;       // 1B (0)
    uint16_t x;             // 발생 위치 정규화 x × 10000
    uint16_t y;             // 발생 위치 정규화 y × 10000
    uint64_t timestamp_ms;  // 8B (Unix epoch 밀리초)
};
#pragma pack(pop)

// 제어 메시지 상수 (서버와 합의된 값)
static constexpr uint16_t kCtrlMagic = 0xDB4C;
static constexpr uint8_t kCtrlRoiSet = 0x01;
static constexpr uint8_t kCtrlRoiClear = 0x02;
static constexpr int kRoiCoordScale = 10000;

// 이벤트 메시지 상수 (서버와 합의된 값)
static constexpr uint16_t kEvtMagic = 0xDB4D;
static constexpr uint8_t kEvtFall = 0x01;

class VideoView;  // 영상+ROI 오버레이 위젯 (videoview.h)
class QPushButton;
class QTabWidget;
class QTableWidget;
class QComboBox;
class QDateEdit;
class QSlider;
class QVBoxLayout;

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
    void updateClock();          // 상단 실시간 시계
    void updateVitals();         // 웨어러블 바이탈 갱신(현재는 시뮬레이션)
    void onRoiButtonClicked();   // "ROI 지정" — 채널 선택 후 그리기 시작
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



private:
    Ui::MainWindow *ui;
    QTcpSocket *socket;
    QByteArray buffer;           // 🌟 명세서 가이드: 수신 데이터를 쌓아둘 버퍼
    VideoView* channelViews[4] = {};  // 4분할 영상+ROI 오버레이 위젯
    bool roiDrawing = false;     // 현재 어느 채널이든 ROI 그리는 중인지

    // ── 대시보드 UI 구성 요소 ──────────────────────────────
    PatientInfo patients[4];     // 병상별 환자 정보
    QLabel* clockLabel = nullptr;      // 상단 실시간 시계
    QLabel* statusDot = nullptr;       // 서버 연결 상태 표시등
    QLabel* statusText = nullptr;      // 서버 연결 상태 문구
    QLabel* liveDots[4];               // 채널별 LIVE 표시등
    QLabel* tempValues[4];             // 채널별 체온 값
    QLabel* hrValues[4];               // 채널별 심박수 값
    QLabel* vitalStatusDots[4];        // 채널별 바이탈 상태등
    QLabel* vitalUpdated[4];           // 채널별 마지막 갱신 시각

    QTimer clockTimer;
    QTimer vitalsTimer;

    // ── TAB 구조 ──────────────────────────────────────────
    QTabWidget* tabWidget = nullptr;

    // TAB2: 비상 로그 조회 및 블랙박스
    QDateEdit* filterDateFrom = nullptr;
    QDateEdit* filterDateTo = nullptr;
    QComboBox* filterRoom = nullptr;
    QComboBox* filterEventType = nullptr;
    QTableWidget* logTable = nullptr;
    QLabel* blackboxPlaceholder = nullptr;
    QSlider* blackboxSeek = nullptr;
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
    QWidget* buildCareTimeDashboard();

    // TAB3 빌드 헬퍼
    QWidget* buildDbTab();
    QWidget* buildResidentSection();
    QWidget* buildResidentForm();
    QWidget* buildCaregiverSection();

    // TAB3 데이터 갱신
    void refreshResidentTable();
    void refreshCaregiverTable();

    // ROI 다각형(정규화 0~1)을 서버로 전송. clear=true면 삭제 메시지.
    void sendRoi(int channel, const QPolygonF& normPts, bool clear = false);

    // 서버 낙상 이벤트 처리 — 채널 강조 + 팝업 (확인 시 강조 해제)
    void handleFallEvent(int channel, quint64 timestampMs);
    bool fallActive[4] = {};   // 채널별 팝업 중복 방지

    // 영상 서버 접속 시도 (최초 접속·재접속 공용)
    void connectToServer();
    QTimer reconnectTimer;     // 연결 끊김 시 자동 재접속 타이머

    QPushButton* roiButton = nullptr;   // "ROI 지정" 버튼
    QPushButton* roiToggleButton = nullptr;  // "ROI 표시" 토글
    QPushButton* micButton = nullptr;        // 🎤 원격 방송(인터콤) — press-and-hold
    QPushButton* alarmClearButton = nullptr; // 경보 해제 (현장 사이렌/LED 끄기)
};

#endif // MAINWINDOW_H