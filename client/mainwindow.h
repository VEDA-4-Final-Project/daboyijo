#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QByteArray>
#include <QLabel>
#include <QTimer>

// 🌟 명세서 3번에 정의된 16바이트 리틀엔디언 구조체 그대로 구현
#pragma pack(push, 1)
struct dbj_vs_header_t {
    uint16_t magic;         // 2B (0xDB4B)
    uint8_t version;        // 1B (0x01)
    uint8_t channel;        // 1B (0~3)
    uint64_t timestamp_ms;  // 8B (Unix epoch 밀리초)
    uint32_t payload_len;   // 4B (JPEG 이미지 크기)
};
#pragma pack(pop)

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

private:
    Ui::MainWindow *ui;
    QTcpSocket *socket;

    QByteArray buffer;           // 🌟 명세서 가이드: 수신 데이터를 쌓아둘 버퍼
    QLabel* channelLabels[4];    // 4분할 화면(영상) 매핑용 배열

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

    // ── UI 빌드 헬퍼 ──────────────────────────────────────
    void buildUi();
    QWidget* buildHeader();
    QWidget* buildVideoWall();
    QWidget* buildVitalsPanel();
    QWidget* buildVideoCard(int channel);
    QWidget* buildVitalCard(int channel);
    void applyTheme();
    void setConnectionState(bool connected, const QString& text);
};
#endif // MAINWINDOW_H
