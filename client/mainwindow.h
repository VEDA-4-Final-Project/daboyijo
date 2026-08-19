#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QSslSocket>
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
#include <QColor>

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
#include "vitaltile.h"



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
// 침대 ↔ 입소자 매핑 — ROI_BIND 헤더 뒤 1개.
struct dbj_roi_bind_t {
    uint8_t  roi_id;        // 채널 안 침대 번호 0~7
    uint8_t  reserved;      // 0
    uint32_t resident_id;   // residents.resident_id (0 = 매핑 해제)
};                          // 6B
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
    uint8_t  roi_id;        // 1B (발생 침대 0~7, 특정 못 하면 kRoiIdNone)
    uint16_t x;             // 2B (발생 위치 정규화 x ×10000)
    uint16_t y;             // 2B (발생 위치 정규화 y ×10000)
    uint64_t timestamp_ms;  // 8B (서버 Unix time ms)
};                          // 18B

// 역방향(서버→클라) 영상검색 결과 — magic=0xDB4E. 헤더 뒤 UTF-8 텍스트(text_len 바이트).
struct dbj_search_result_header_t {
    uint16_t magic;         // 2B (0xDB4E)
    uint8_t  version;       // 1B (0x01)
    uint8_t  channel;       // 1B (0~3, 요청한 채널)
    uint32_t text_len;      // 4B (이어지는 UTF-8 답변 텍스트 바이트 수)
};                          // 8B
#pragma pack(pop)

// 제어 메시지 상수 (서버와 합의된 값 — protocol/video_stream.h와 동일하게 유지)
static constexpr uint16_t kCtrlMagic = 0xDB4C;
static constexpr uint8_t kCtrlRoiSet = 0x01;
static constexpr uint8_t kCtrlRoiClear = 0x02;
static constexpr uint8_t kCtrlCameraSet = 0x05;    // 채널 카메라 연결 (헤더 뒤 RTSP URL)
static constexpr uint8_t kCtrlCameraClear = 0x06;  // 채널 카메라 해제
static constexpr uint8_t kCtrlImageSet = 0x07;     // 카메라 이미지 파라미터 (헤더 뒤 dbj_image_params_t)
static constexpr uint8_t kCtrlFocusSet = 0x08;     // 카메라 포커스 (헤더 뒤 dbj_focus_t)
static constexpr uint8_t kCtrlRoiBind = 0x09;      // 침대 ↔ 입소자 매핑 (헤더 뒤 dbj_roi_bind_t)
static constexpr uint8_t kCtrlSearchQuery = 0x0A;  // 영상검색 질의 (헤더 뒤 UTF-8 질의 문자열)
static constexpr int kSearchQueryMax = 300;        // DBJ_SEARCH_QUERY_MAX
static constexpr uint8_t kChannelAll = 0xFF;       // SEARCH_QUERY channel 자리의 "전체 채널" (DBJ_CHANNEL_ALL)
static constexpr uint8_t kFocusWhole = 0;          // 전체 자동초점
static constexpr uint8_t kFocusArea = 1;           // 클릭 영역 초점
static constexpr int kRoiCoordScale = 10000;
// roi_id 자리의 특수값 — 삭제 시 "그 채널 전부", 이벤트에선 "침대 미상".
// (서버 DBJ_ROI_ID_ALL / DBJ_ROI_ID_NONE 과 같은 값으로 유지할 것)
static constexpr int kRoiIdAll = 0xFF;
static constexpr int kRoiIdNone = 0xFF;
static constexpr int kCameraUrlMax = 512;          // DBJ_CAMERA_URL_MAX

// 이벤트 메시지 상수 (서버 스펙)
static constexpr uint16_t kEvtMagic = 0xDB4D;
static constexpr uint8_t kEvtFall = 0x01;       // 낙상 확정
static constexpr uint8_t kEvtBedEgress = 0x02;  // 침대 이탈
static constexpr uint8_t kEvtVitalAbnormal = 0x03;  // 웨어러블 생체데이터 이상 (x,y 미사용)

// 영상검색 결과 메시지 상수 (서버 스펙)
static constexpr uint16_t kSearchMagic = 0xDB4E;

#include "videoview.h"   // RoiZone / kMaxRoiZones — 침대 목록을 값으로 들고 있어 필요

class FramePreview;  // 채널 스트립 썸네일 (videoview.h)
class WipeCompare;   // 이미지 탭 적용 전/후 와이프 비교 (videoview.h)
class Sparkline;  // 심박 미니 추세 그래프 (sparkline.h)
class QDialog;
class QPushButton;

class QTableWidget;
class QComboBox;
class AlertMatrixPreview;
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
class QCalendarWidget;
class QHBoxLayout;
class ActivityChart;
class QTreeWidget;
class QTreeWidgetItem;
class TimelineBar;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// 채널(=병상)별 환자 / 웨어러블 정보 묶음
struct PatientInfo {
    QString name;   // 환자 이름
    QString bed;    // 위치 표기 — 병실/침대 제거 후 "채널 N"을 담는다(오버레이/바이탈 공용)
    QString room;   // 호실 — MQTT 알림 명령에 실어 보낸다(알림 노드가 LED에 띄운다)
};

// NVR(연속녹화) 세그먼트 1개 — /list 응답 파일명(ch{N}_{startMs}.mp4)을 파싱한 결과.
// 콤보박스로 채널 필터링할 때 재요청 없이 이 목록에서 다시 골라 쓴다.
struct NvrSegmentInfo {
    int channel = -1;
    qint64 startMs = 0;
    QString url;
};

// 이벤트 기록 표의 한 줄. DB 원장(events)에서 읽어온 것과 실시간으로 도착한 것이
// 같은 모양을 갖도록 한 곳에 정의한다 — 두 경로가 따로 표를 채우면 열 순서·색·
// 클립 URL 규칙이 조용히 갈라진다.
struct EventLogRow {
    qint64  eventId = -1;      // events.event_id (-1 = 방금 소켓으로 온 것, 아직 조회 전)
    qint64  occurredMs = 0;
    int     channel = -1;
    QString typeCode;          // FALL / EGRESS / VITAL_ABNORMAL (DB ENUM과 같은 값)
    QString source;            // CAMERA / WEARABLE
    QString residentName;      // 비어 있으면 "미상"으로 표시
    QString place;             // "101호 · 채널 2 · 침대 1"
    QString clipUrl;           // 호스트까지 붙인 완성 URL (비어 있으면 재생 불가)
    bool    confirmed = false;
    QString confirmedBy;
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
    void updateCareTime();       // 선택한 날짜·입소자의 리포트 지표 재조회
    // 달력에서 날짜를 고르면 그 날짜 리포트로 다시 조회한다.
    void onReportDateChanged(const QDate& date);
    // 상단 이름 탭에서 입소자를 고르면 그 사람 리포트로 전환한다.
    void onReportResidentChanged(int residentId);
    void onRoiButtonClicked();   // "침대 추가" — 선택 채널에 새 침대 그리기 시작
    void onRoiClearClicked();    // "침대 제거" — 선택 침대(없으면 채널 전체) 삭제
    void onRoiVisibilityToggled(bool on);  // "ROI 표시" 토글
    // 그리기 완료 → 서버 전송 + 로컬 목록 반영
    void onRoiCompleted(int channel, int roiId, const QPolygonF& normPts);
    void onRoiZoneSelected(int channel, int roiId);  // 영상에서 침대 클릭 → 목록 강조
    void onMicToggled(bool on);  // 마이크 버튼 토글 — 한 번 클릭하면 방송 시작, 다시 누르면 종료
    void onAlarmClearClicked();  // 경보 해제
    void onAddCameraClicked();   // "카메라 연결" — CCTV IP 입력 → 서버로 전송
    void onSearchCameraClicked();// "카메라 검색" — ONVIF WS-Discovery로 같은 망 카메라 탐색
    void onCameraClearClicked(); // "카메라 해제" — 모든 채널 CAMERA_CLEAR 전송
    // 소켓은 붙어있는데 카메라 쪽 스트림만 멈추는 경우(setLive(true)는 프레임
    // 도착 때만 불려서 이런 정지를 아무도 알려주지 않는다) — 주기적으로 마지막
    // 프레임 시각을 점검해 LIVE 배지를 "미연결"로 되돌린다.
    void checkChannelHealth();

    // ── MQTT (웨어러블·알림 노드) ─────────────────────────
    // 영상 경로(TCP)와 별개로, 브로커를 통해 들어오는 것들을 받는 슬롯.
    void onWearableData(const WearableData& data);   // 생체·낙상 원본 도착
    void onMqttAlarm(const AlarmCommand& cmd);       // 알림 노드로 나간 명령을 엿들음
    void onMqttConnected();
    void onMqttDisconnected();
    void onMqttError(const QString& message);
    void onAlarmNodeStatus(const QString& node, bool online);   // 알림 노드 온라인/오프라인

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
    // QSslSocket은 QTcpSocket 파생이라 나머지 코드(sock->state()/write() 등)는
    // QTcpSocket*로 받아써도 그대로 동작 — TLS 전용 API(connectToHostEncrypted 등)를
    // 쓰는 지점(connectToServer)만 QSslSocket*가 필요하다.
    QSslSocket *sockets[kNumServers] = {};
    QByteArray buffers[kNumServers];   // 연결마다 바이트 스트림이 별개 → 버퍼도 분리
    QTcpSocket* socketForChannel(int ch) { return sockets[serverForChannel(ch)]; }
    // ── 확장 지점 메모(room 개념 도입, 2026-08) ──────────────────────
    // 이 파일의 고정크기-4 배열들(channelViews/videoCards/patients/
    // residentsByChannel_ 등 총 19곳)과 setVideoFocus()의 2×2 그리드 배치는
    // 전부 "카메라 1대(4채널) = 방 1개"를 전제한다. 카메라를 더 붙여 방이
    // 여러 개가 되는 걸 실제로 지원하려면 이 배열들을 채널 인덱스가 아니라
    // (room, 채널) 쌍 기반의 동적 컨테이너로, setVideoFocus()도 데이터 기반
    // 레이아웃으로 재작성해야 한다 — 지금은 리스크 대비 이득이 낮아 보류.
    // room "이름" 자체는 이미 표시 레이어(currentRoomName(), mainwindow.cpp)에
    // 도입돼 있어 방을 늘릴 때 그 값부터 목록으로 확장하면 된다.
    VideoView* channelViews[4] = {};  // 4분할 영상+ROI 오버레이 위젯
    QWidget* videoCards[4] = {};      // 영상 카드(스포트라이트 재배치용)
    QGridLayout* videoGrid = nullptr; // 영상 월 그리드(재배치 대상)
    // 마지막으로 실제 적용한 배치의 서명. relayoutGrid()가 이 값과 비교해
    // 달라졌을 때만 위젯을 옮긴다 — 채널을 고를 때마다 4장을 떼었다 붙이면
    // 영상이 깜빡인다. -1은 "아직 한 번도 배치 안 함".
    int gridKey_ = -1;
    // 낙상·침상이탈 감지 시 그 채널을 크게, 나머지는 작게. -1이면 균등 2×2로 복귀.
    void setVideoFocus(int channel);
    // ── Wisenet Viewer 스타일 관제 화면 구성 (2026-08) ─────────────
    // 화면을 세 덩어리로 나눈다: [리소스 트리] [레이아웃 탭 + 영상 그리드 +
    // 타임라인] [바이탈 패널]. 그리드 배치는 "프리셋(gridLayout_) × 선택 채널
    // (selectedChannel_) × 숨긴 타일(tileHidden_)" 세 상태의 함수이며,
    // 그 셋 중 무엇이 바뀌든 relayoutGrid() 한 곳에서만 배치가 결정된다 —
    // 배치 코드가 여러 갈래로 흩어지면 경보 스포트라이트와 사용자가 고른
    // 레이아웃이 서로를 덮어써 어긋난다.
    enum class GridLayout { Quad, Spotlight, Single };
    GridLayout gridLayout_ = GridLayout::Quad;
    int  selectedChannel_ = 0;      // 지금 조작 대상 채널(선택 강조색 테두리)
    bool tileHidden_[4] = {};       // 타일 ×로 레이아웃에서 뺀 채널(카메라 해제 아님)
    void relayoutGrid();            // 위 세 상태 → videoGrid 실제 배치
    void setGridLayout(GridLayout mode);
    void selectChannel(int ch);     // 타일 선택 + 리소스 트리 강조 동기화
    void setTileHidden(int ch, bool hidden);
    QString channelDisplayName(int ch) const;   // "CH1 · 김복순" (타일/트리 공용)

    // 좌측 리소스 패널 — Root > 그룹 > CH01~04 + 레이아웃 프리셋.
    QWidget* buildResourcePanel();
    void     refreshResourceTree();          // 카메라 연결/입소자 변경 시 라벨·색 갱신
    void     setResourceCollapsed(bool on);
    QFrame*          resourcePanel_ = nullptr;
    QTreeWidget*     resourceTree_ = nullptr;
    QLineEdit*       resourceSearch_ = nullptr;
    QPushButton*     resourceToggle_ = nullptr;
    QWidget*         resourceBody_ = nullptr;   // 펼쳤을 때 보이는 부분(검색+트리)
    QLabel*          resourceHead_ = nullptr;   // "리소스" 제목 — 접으면 숨긴다
    // 접었을 때 대신 보이는 세로 칩 레일(CH1~4). 44px 빈 막대만 남기면 무엇을
    // 접었는지도, 어떻게 펴는지도 알 수 없다 — 채널 상태는 접어도 남긴다.
    QWidget*         resourceRail_ = nullptr;
    QPushButton*     resChipBtns_[4] = {};
    bool             resourceCollapsed_ = false;
    QTreeWidgetItem* camItems_[4] = {};

    // 그리드 위 레이아웃 탭 + 타일에 얹는 크롬(닫기 버튼 / 호버 툴바)
    QWidget*     buildLayoutTabs();
    QWidget*     buildTileChrome(int channel, QWidget* card);
    // 크롬(닫기·툴바)은 레이아웃 밖 자식이라 카드 크기에 맞춰 직접 옮겨 준다.
    void         layoutTileChrome(int channel);
    void         saveChannelSnapshot(int channel);   // 타일 툴바 스냅샷 저장
    QPushButton* layoutTabBtns_[3] = {};
    QPushButton* tileCloseBtns_[4] = {};
    QWidget*     tileToolbars_[4] = {};

    // ── 하단 타임라인 + 재생바 ──────────────────────────────────
    // 라이브/재생 전환은 영상 영역을 통째로 갈아끼운다(liveOrPlaybackStack_):
    // 0 = 실시간 그리드, 1 = NVR 재생 화면. 소켓 프레임은 라이브에서만 보이고
    // 재생 중에도 계속 수신은 된다 — 되돌아오면 곧바로 최신 화면이 뜬다.
    QWidget* buildTransportBar();
    void     refreshTimeline();              // NVR 세그먼트·이벤트 → 타임라인 반영
    void     setPlaybackMode(bool on);
    void     seekPlaybackTo(qint64 ms);      // 그 시각을 담은 세그먼트를 찾아 재생
    TimelineBar*    timeline_ = nullptr;
    QStackedWidget* liveOrPlaybackStack_ = nullptr;
    QVideoWidget*   playbackVideo_ = nullptr;
    // 재생 모드로 막 넘어왔을 때(아직 시각을 안 고른 상태)와 그 시각에 녹화가
    // 없을 때 보여주는 안내. 검은 화면만 띄우면 "고장인가"로 읽힌다.
    QLabel*         playbackPlaceholder_ = nullptr;
    QMediaPlayer*   playbackPlayer_ = nullptr;
    QPushButton*    liveModeBtn_ = nullptr;
    QPushButton*    playbackModeBtn_ = nullptr;
    QPushButton*    transportPlayBtn_ = nullptr;
    QLabel*         transportTimeLabel_ = nullptr;
    QComboBox*      transportSpeedCombo_ = nullptr;
    QTimer          timelineTimer_;          // 라이브일 때 창을 "지금"에 맞춰 미는 타이머
    bool            playbackMode_ = false;
    qint64          playbackSegStartMs_ = 0; // 지금 재생 중인 세그먼트의 시작 시각
    // 낙상·침상이탈이 난 시각 — 타임라인 마커. 이벤트 로그와 별개로 가볍게 들고 있는다.
    struct TimelineEvent { qint64 atMs; int channel; QColor color; };
    QVector<TimelineEvent> timelineEvents_;
    void pushTimelineEvent(int channel, qint64 atMs, const QColor& color);

    // 채널별 마지막 카메라 RTSP URL — Pi가 잠깐 끊겼다 붙을 때 자동 재전송용(세션 한정,
    // 비밀번호가 포함돼 QSettings엔 저장하지 않는다). 비어 있으면 미연결.
    QString lastCameraUrl_[4];
    // 채널별 카메라 연결 여부(QSettings 지속) — 서버는 Qt를 껐다 켜도 스트리밍을
    // 유지하므로, 재시작 후 URL이 없어도 이 플래그로 "해제" 대상을 안다. 비어 있으면 미연결.
    bool cameraActive_[4] = {};
    // 채널별 마지막 영상 프레임 수신 시각(에폭 ms). 0이면 이번 세션에서 아직
    // 한 장도 못 받음. checkChannelHealth()가 이 값으로 "신호 끊김"을 판정한다.
    qint64 lastFrameMs_[4] = {};
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
    //   rebuildVitalCards() 가 구성 변경 시에만 diff로 생성·삭제한다(D-04).
    QHash<int, VitalTile*> vitalTiles_;     // 키 → 타일 인스턴스(단일 출처)

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
    QVBoxLayout* vitalListLayout_ = nullptr;  // 바이탈 카드 목록(재생성 대상)

    QTimer clockTimer;
    QTimer vitalsTimer;
    QTimer careTimeTimer;        // 케어 타임 대시보드 주기 갱신(care_logs 재조회)
    QTimer reconnectTimer;       // 영상 서버 자동 재접속
    QTimer channelHealthTimer;   // 채널별 프레임 정지(신호 끊김) 감시

    // ── TAB 구조 ──────────────────────────────────────────
    // ── 좌측 네비 레일 + 본문 스택 (예전 상단 QTabWidget 대체) ──
    static constexpr int kNavCount = 6;
    static constexpr int kNavEventLog = 1;  // 영상 재생기(블랙박스/NVR)가 있는 페이지 — 다른 페이지에서 클립 재생 시 이동 대상
    QWidget* buildNavRail();          // 레일 구성(아이콘+라벨 6개 + 접기 토글)
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
    // 이벤트 기록 표의 열 순서. 인덱스를 코드 곳곳에 숫자로 흩뿌리면 열을 하나
    // 끼워 넣을 때마다 조용히 어긋나므로 이름으로 고정한다.
    enum LogCol { LogWhen = 0, LogType, LogPlace, LogResident, LogSource, LogStatus,
                  LogColCount };
    // 0열 아이템에 실어 두는 부가 데이터. Qt::UserRole+N 을 직접 쓰지 않는다.
    enum LogRole { LogClipUrl = Qt::UserRole, LogChannel, LogTimestamp, LogEventId };

    QComboBox* filterChannel = nullptr;     // 전체 채널 / CH1~4
    QComboBox* filterConfirmed = nullptr;   // 전체 / 미확인만 / 확인만
    QLabel*    logCountLabel = nullptr;     // "N건 · 미확인 M건"
    QLabel*    logEmptyHint = nullptr;      // 조회 결과 0건일 때 표 위에 띄우는 안내
    QLabel*    eventContextLabel = nullptr; // 우측 재생기 위 "누가 · 언제 · 어디서"

    // events 원장을 현재 필터 조건으로 조회해 표를 통째로 다시 채운다.
    // 이 함수가 표의 유일한 채움 경로다(실시간 도착분만 insertEventRow로 덧붙는다).
    void reloadEventLog();
    // 표에 한 줄 넣기 — DB 조회분/실시간 도착분 공용.
    void insertEventRow(const EventLogRow& e);
    // 실시간 이벤트를 표에 얹는다(서버는 같은 이벤트를 DB에도 쓴다 — 다음 조회 때 합쳐진다).
    void appendLiveEvent(int channel, int roiId, qint64 occurredMs,
                         const QString& typeCode, const QString& source);
    // 종류 코드 → 화면 문구/색. 표·필터·요약이 같은 사전을 본다.
    static QString eventTypeLabel(const QString& code);
    static QString eventSourceLabel(const QString& code);
    // 결과 건수·미확인 수 갱신 + 빈 상태 안내 표시
    void refreshLogSummary();
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
    qint64 blackboxPendingSeekMs_ = -1;   // durationChanged 이후 한 번 적용할 탐색 위치(-1=없음)

    // NVR(연속녹화) 세그먼트 목록. 화면에 목록으로 띄우지는 않는다 — 관제화면
    // 하단 타임라인이 이 데이터를 시간축 위에 그리고, 이벤트 기록의
    // [이 시점 NVR에서 이어보기]가 여기서 해당 세그먼트를 찾아 쓴다.
    QVector<NvrSegmentInfo> nvrSegments_;

    // 이벤트↔NVR 연결: 로그에서 마지막으로 연 이벤트의 채널/시각(NVR 시점 점프용)
    int selectedEventChannel_ = -1;
    qint64 selectedEventTimestampMs_ = -1;
    QPushButton* nvrJumpButton = nullptr;
    QPushButton* clipDownloadButton = nullptr;

    // ── 영상검색(🔍) — 좌측 네비의 독립 페이지(실제 VMS의 Search 섹션처럼).
    //    케어봇(video_search_module)과 같은 서버 로직 재사용. 질의는
    //    DBJ_CTRL_SEARCH_QUERY, 응답은 DBJ_SEARCH_MAGIC.
    QComboBox* searchChannelCombo = nullptr;
    QLineEdit* searchQueryEdit = nullptr;
    QPushButton* searchButton = nullptr;

    // 검색 결과 한 건. 서버가 돌려주는 답변은 자유 문장이 아니라
    //   "· 2026-08-18 11:06 · 채널 4 · 낙상 · 전승현님" + 다음 줄에 클립 URL
    // 형식의 목록이라(video_search_module.cpp), 그대로 파싱해 리스트로 만든다.
    struct SearchHit {
        QString when;    // "2026-08-18 11:06"
        QString meta;    // "채널 4 · 낙상 · 전승현님"
        QString url;     // 클립 주소(없을 수 있다)
    };
    QListWidget*    searchResultList_ = nullptr;
    QLabel*         searchCountLabel_ = nullptr;
    QLabel*         searchMessage_ = nullptr;   // 결과가 목록이 아닐 때(안내·오류) 표시
    QLabel*         searchContext_ = nullptr;   // 재생기 위 "언제 · 무슨 일"
    // 재생기는 이 페이지 전용이다. 이벤트 기록 페이지의 재생기(blackboxPlayer)를
    // 빌려 쓰려면 그쪽 페이지로 이동해야 해서, 검색하다 말고 화면이 튀었다.
    QStackedWidget* searchPlayerStack_ = nullptr;   // 0=안내 / 1=영상
    QVideoWidget*   searchVideo_ = nullptr;
    QMediaPlayer*   searchPlayer_ = nullptr;
    QSlider*        searchSeek_ = nullptr;
    QPushButton*    searchPlayPause_ = nullptr;
    QLabel*         searchTimeLabel_ = nullptr;
    bool            searchSeeking_ = false;     // 사용자가 재생바를 잡고 있는 중
    // 서버 답변 → 결과 목록. 목록으로 볼 수 없는 답변이면 message 에 원문을 담는다.
    static QVector<SearchHit> parseSearchReply(const QString& text, QString* message);
    void playSearchClip(const QString& url, const QString& context);
    QWidget* buildVideoSearchTab();          // 네비 "영상 검색" 페이지 전체
    void sendSearchQuery();
    // onReadyRead가 DBJ_SEARCH_MAGIC 패킷을 다 모으면 호출 — 답변 표시 + 버튼 복구
    void onSearchResultReceived(int channel, const QString& text);
    // ── 일일 리포트: 날짜 선택 ──────────────────────────────
    // 리포트는 "특정 날짜 + 특정 입소자" 단위다. 그 날짜를 고르는 곳.
    // 여기서 고른 날짜를 updateCareTime()을 비롯한 모든 집계 쿼리가 함께 본다
    // (예전엔 CURDATE() 하드코딩이라 오늘치만 볼 수 있었다).
    QCalendarWidget* reportCalendar = nullptr;
    QLabel* reportDateLabel = nullptr;   // 우측 상단 "2026-08-13 (목)"
    QDate   reportDate_ = QDate::currentDate();
    QWidget* buildReportCalendar();      // 좌측 날짜 선택 칼럼
    // 달력 요일 머리글/주말 색 — QSS가 닿지 않는 부분이라 코드로 칠한다.
    void     applyCalendarPalette(QCalendarWidget* cal);

    // ── 일일 리포트: 입소자 선택 + 지표 ────────────────────
    // 리포트는 "날짜 + 사람" 한 명 단위다(PDF 한 장, AI 요약 한 문단이 그 단위).
    // 채널이 아니라 입소자 단위로 두는 이유: 침대마다 사람을 매핑하면 한 채널에
    // 여러 명이 들어와 채널 4칸 고정이 맞지 않는다.
    QWidget* buildReportDetail();        // 우측: 이름 탭 + 지표 타일
    void     reloadReportResidents();    // 재원 입소자로 이름 탭을 다시 만든다
    QHBoxLayout*        residentTabLayout = nullptr;   // 이름 탭이 붙는 줄
    QVector<QPushButton*> residentTabBtns;             // 탭 버튼(선택 표시 갱신용)
    QVector<int>          residentTabIds;              // 버튼과 같은 순서의 resident_id
    int      reportResidentId_ = -1;     // 지금 보고 있는 입소자 (-1 = 없음)
    QLabel*  reportResidentMeta = nullptr;             // "101호 · 3번침대"
    // 지표 타일 4개 — 큰 값 + 아래 보조 문구
    QLabel* tileLyingVal = nullptr;    QLabel* tileLyingSub = nullptr;
    QLabel* tileActivityVal = nullptr; QLabel* tileActivitySub = nullptr;
    QLabel* tileCareVal = nullptr;     QLabel* tileCareSub = nullptr;
    QLabel* tileEventVal = nullptr;    QLabel* tileEventSub = nullptr;
    // 24시간 활동량 그래프 (걸음 막대 + 심박 선). 값은 updateCareTime()이 넣는다.
    ActivityChart* activityChart = nullptr;

    // ── 리포트 지표 스냅샷 ────────────────────────────────────
    // updateCareTime()이 계산한 값을 그대로 보관한다. PDF·AI 요약이 이걸 읽어야
    // 화면에 보이는 숫자와 문서에 찍히는 숫자가 반드시 같아진다 — 각자 다시
    // 조회하면 그 사이 데이터가 바뀌었을 때 "화면은 57분, PDF는 58분"이 된다.
    struct ReportMetrics {
        bool    valid = false;
        QString residentName, residentMeta;
        int     lyingSec = 0,  lyingCount = 0;
        int     steps = 0,     activeMin = 0;
        int     careSec = 0,   careCount = 0;
        QString careLast;                      // "20:16" (없으면 빈 문자열)
        int     eventTotal = 0;
        QString eventDetail;                   // "낙상1 · 생체1"
    };
    ReportMetrics metrics_;
    void exportReportPdf();      // [PDF 내보내기] 버튼 — 이 날짜·이 입소자 한 장

    // ── AI 요약 (Gemini) ──────────────────────────────────────
    // ★ 리포트의 숫자는 전부 SQL 이 만든다. AI 는 그 완성된 수치를 받아 문장만 쓴다.
    //   AI 에게 계산을 시키면 같은 날짜를 두 번 열 때 값이 달라지고 근거도 못 댄다.
    //   그래서 프롬프트에는 metrics_ 의 집계값만 넣고, 원본 로그는 보내지 않는다
    //   (개인정보 노출도 줄고 토큰도 아낀다).
    void requestAiSummary();          // [AI 요약] 버튼 — 없으면 생성, 있으면 캐시 표시
    void loadCachedSummary();         // 날짜·입소자가 바뀔 때 daily_reports 에서 읽기
    void setSummaryText(const QString& text, bool cached);
    QString geminiApiKey() const;     // QSettings 에 저장된 키(없으면 빈 문자열)
    QLabel*      summaryLabel = nullptr;   // 요약 문장 표시
    QPushButton* summaryBtn = nullptr;     // [AI 요약] / [다시 생성]
    bool         summaryBusy_ = false;     // 중복 요청 방지

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


    // 상세/편집 폼 — 기본정보 (침대는 제거, 위치는 호실·카메라 채널로 표기)
    QLineEdit* editName       = nullptr;
    QLineEdit* editRoom       = nullptr;   // 호실 배정 (residents.room)
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
    VitalTile* buildVitalCard(int key, const QString& name, const QString& bedText);
    // 바이탈 카드 목록을 입소자 구성에 맞춰 다시 만든다. 구성이 바뀔 때만
    // 생성·삭제하고, 살아남은 타일은 setIdentity()로만 갱신한다(D-04).
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
    void playBlackboxClipAt(const QString& url, qint64 seekMs);  // 재생 후 지정 위치로 탐색
    void refreshNvrSegments();         // 각 Pi의 /list(NVR 포트)를 받아 nvrSegments_ 갱신(→ refreshTimeline)
    void jumpToNvrContext();           // 선택된 로그 이벤트 시점의 NVR 세그먼트를 찾아 그 위치로 재생
    void downloadCurrentClip();        // 현재 재생 중인 클립을 로컬에 저장
    void markLogConfirmed(int row);                // 영상 확인 → 상태 '확인'(초록) 마킹
    void applyLogFilters(bool withDates = false);  // 로그 표 필터링(이벤트/날짜)
    // 로그가 바뀔 때마다 행 색(이벤트/상태 배지)을 다시 칠하고 요약 카드 값을 갱신한다.
    void refreshEventLog();

    // 일일 리포트 — 날짜별/입소자별 지표(달력 + 상세).
    QWidget* buildReportPage();


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

    // 낙상 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동.
    // roiId는 서버가 밝힌 발생 침대(=누구). 특정 못 했으면 -1.
    void handleFallEvent(int channel, int roiId, quint64 timestampMs,
                         float nx, float ny);

    // 침상 이탈 이벤트 처리 — 빨간색 테두리 + 비상 로그 추가 + 블랙박스 연동
    void handleBedEgressEvent(int channel, int roiId, quint64 timestampMs,
                              float nx, float ny);

    // 이벤트 표시용 문구 — 침대를 특정 못 했으면 "신원 미상"으로 정직하게 쓴다
    QString eventWhoLabel(int channel, int roiId) const;
    QString eventPlaceLabel(int channel, int roiId) const;
    void handleVitalAbnormalEvent(int channel, quint64 timestampMs);

    // ── 침대 ROI (채널당 여러 개) ──────────────────────────────
    // 한 채널에 침대가 여럿이고 침대마다 입소자가 다르다. 화면 표시의 진짜 소스는
    // 여기이고, VideoView들에는 refreshRoiZones()가 이름표를 붙여 밀어 넣는다.
    QVector<RoiZone> roiZones_[4];       // 채널 → 침대 목록(라벨은 비워 둔 원본)
    QHash<int, int>  roiResident_[4];    // 채널 → (침대 번호 → resident_id)

    // 침대 ROI 다각형(정규화 0~1)을 서버로 전송. clear=true면 삭제 메시지이고,
    // 그때 roiId에 kRoiIdAll을 주면 그 채널의 침대를 전부 지운다.
    void sendRoi(int channel, int roiId, const QPolygonF& normPts, bool clear = false);
    // 침대 ↔ 입소자 매핑을 서버로 전송 (residentId=0이면 해제).
    void sendRoiBind(int channel, int roiId, int residentId);
    // 침대 목록을 오버레이·4분할·인스펙터·배지에 다시 반영
    void refreshRoiZones(int channel);
    // roi_zones 표에서 침대·입소자 매핑을 복원 (서버도 같은 표를 읽는다)
    void loadRoiZonesFromDb();
    // 그 침대에 매핑된 입소자 이름 (없으면 "미지정")
    QString zoneResidentName(int channel, int roiId) const;
    // 인스펙터의 침대 목록(번호 · 입소자 콤보 · 삭제)을 다시 만든다
    void rebuildBedList();
    QVBoxLayout* bedListLayout_ = nullptr;   // 침대 목록이 붙는 컨테이너
    QLabel*      bedListEmpty_ = nullptr;    // "아직 침대가 없습니다" 안내

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
    QPushButton* micButton = nullptr;        // 🎤 원격 방송(인터콤) — 클릭 토글
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

    // ── 장치 설정 래퍼 (좌측 네비 "장치 설정" = 카메라 + 알림 서브탭) ──
    // 카메라 설정(연결/ROI/이미지)과 알림 설정은 성격이 달라, 상단 [카메라][알림]
    // 세그먼트로 나눈 한 페이지 안에 스택으로 담는다. index5 = 이 래퍼.
    QWidget* buildDeviceSettingsTab();       // contentStack 의 장치 설정 페이지(1회)
    QWidget* buildDeviceModeSegment();       // 상단 [카메라][알림] 세그먼트
    QWidget* deviceSettingsTab_ = nullptr;
    QStackedWidget* deviceStack_ = nullptr;  // 0=카메라 / 1=알림
    QPushButton* deviceModeBtns_[2] = {};    // [0]카메라 [1]알림

    // ── 알림 노드 설정 (장치 설정 → 알림 서브탭) ──
    // veda/alarm/control 로 밝기(0~255)/음량(0~100)을 노드에 보낸다. 미리보기는
    // 실제 64x32 패널을 흉내 내며 밝기 슬라이더에 즉시 반응한다.
    QWidget* buildAlertSettingsTab();
    AlertMatrixPreview* alertPreview_ = nullptr;
    ClickSlider* alertBright_ = nullptr;     // 0~100 (→255 매핑해 전송)
    ClickSlider* alertVol_    = nullptr;     // 0~100
    QComboBox*   alertNode_   = nullptr;     // 대상 알림 노드 id
    QLabel*      alertApplied_ = nullptr;    // "마지막 적용 HH:MM:SS"
    QLabel*      alertStatusBadge_ = nullptr;      // 대상 노드의 온라인/오프라인 배지
    QMap<QString, bool> alertNodeOnline_;          // node_id → online(마지막으로 받은 상태)
    void refreshAlertStatusBadge();                // 배지 텍스트·색을 alertNodeOnline_ 에 맞춰 갱신

    // ── 카메라 이미지 조절 (밝기/대비/채도) ──────────────────────
    // 슬라이더 값을 IMAGE_SET 제어 메시지로 서버에 보내면, 서버가 ONVIF Imaging으로
    // 실제 카메라에 적용한다. 대상 채널은 공용 채널(roiEditChannel)을 따른다.
    void     sendImageParams(int channel, int b, int c, int s);
    // 카메라 초점 — area=false 전체 자동초점, true 클릭 지점(nx,ny 정규화 0~1) 영역 초점.
    void     sendFocus(int channel, bool area, float nx, float ny);
    // 영상 타일 호버 툴바 표시/숨김 + 타일 크롬 재배치.
    bool     eventFilter(QObject* obj, QEvent* ev) override;
    ClickSlider* imgBright = nullptr;
    ClickSlider* imgContrast = nullptr;
    ClickSlider* imgSaturation = nullptr;
    // 이미지/초점 조작은 카메라가 붙어 있을 때만 의미가 있다. 미연결 채널에서
    // 눌러도 아무 일도 일어나지 않으므로, 눌리는 것처럼 보이게 두지 않는다.
    QPushButton* imgApplyBtn = nullptr;
    QPushButton* imgResetBtn = nullptr;
    QPushButton* imgFocusBtn = nullptr;
    QLabel*      imgDisabledHint = nullptr;   // 왜 못 만지는지 알려주는 한 줄
    // 채널별 마지막 적용값(밝기/대비/채도). 카메라의 실제 현재값을 읽어오는 게
    // 아니라 "이 PC에서 마지막으로 보낸 값"이다 — 프로토콜에 조회가 없다.
    // 채널을 오갈 때 값이 따라오게 하고, 앱을 껐다 켜도 남는다.
    void loadImageParams(int channel);        // QSettings → 슬라이더
    void saveImageParams(int channel);        // 슬라이더 → QSettings
    // 현재 선택 채널의 연결 여부에 맞춰 ROI/이미지 컨트롤을 켜고 끈다.
    void refreshCamControlsEnabled();
    // 연결 탭의 4채널 상태 요약 배지(CH1~4)
    QLabel* camConnBadges[4] = {};
    // 적용 전/후 와이프 비교 화면(영상 1장 + 드래그 구분선).
    // 두 장을 따로 놓으면 각 장이 절반으로 줄고 카드에 여백이 남아, 다른 탭과
    // 틀도 어긋났다. 한 장으로 겹치면 연결·ROI 탭의 큰 영상과 크기가 같아진다.
    WipeCompare* imgWipe_ = nullptr;
    void setImagePreviewFrame(const QPixmap& pm);      // 실시간 프레임 주입
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
