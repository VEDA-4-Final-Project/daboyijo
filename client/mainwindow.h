#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QByteArray>
#include <QLabel>

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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onReadyRead(); // 명세서 가이드라인 구현 슬롯

private:
    Ui::MainWindow *ui;
    QTcpSocket *socket;

    QByteArray buffer;          // 🌟 명세서 가이드: 수신 데이터를 쌓아둘 버퍼
    QLabel* channelLabels[4];   // 4분할 화면 매핑용 배열
};
#endif // MAINWINDOW_H