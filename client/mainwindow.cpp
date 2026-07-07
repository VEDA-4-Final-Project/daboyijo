#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QHostAddress>
#include <QPixmap>
#include <QDateTime>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 1. UI 디자인에서 배치한 QLabel 4개 매핑
    channelLabels[0] = ui->labelChannel0;
    channelLabels[1] = ui->labelChannel1;
    channelLabels[2] = ui->labelChannel2;
    channelLabels[3] = ui->labelChannel3;

    // 2. 소켓 생성 및 시그널 연결
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);

    // 3. 명세서 스펙: 5500번 포트로 즉시 접속 (IP 주소는 RPi 주소 입력)
    socket->connectToHost(QHostAddress("172.20.35.87"), 5500);
    qDebug() << "라즈베리파이 영상 서버(Port: 5500) 접속 시도 중...";
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::onReadyRead()
{
    // 🌟 명세서 가이드: 들어온 데이터를 무조건 글로벌 버퍼 뒤에 붙임
    buffer.append(socket->readAll());

    // 버퍼에 데이터가 남아있는 동안 무한 반복 파싱
    while (true) {
        // 1) 헤더 크기(16바이트)만큼도 안 모였으면 데이터 더 올 때까지 대기
        if (buffer.size() < (int)sizeof(dbj_vs_header_t))
            return;

        // 2) 헤더 영역 복사 (리틀엔디언 환경이므로 memcpy로 충분)
        dbj_vs_header_t header;
        memcpy(&header, buffer.constData(), sizeof(header));

        // 3) 명세서 가이드: 매직넘버(0xDB4B) 검증, 다르면 스트림 어긋난 것
        if (header.magic != 0xDB4B) {
            qDebug() << "⚠️ 스트림 어긋남! 연결을 끊고 재접속을 시도합니다.";
            socket->disconnectFromHost();
            buffer.clear(); // 오염된 버퍼 초기화
            return;
        }

        // 4) 전체 패킷 크기 계산 = 헤der(16B) + 진짜 JPEG 크기
        int total = sizeof(header) + header.payload_len;

        // JPEG 데이터가 아직 다 안 왔으면 다음 readyRead 때까지 대기
        if (buffer.size() < total)
            return;

        // 5) 🌟 명세서 가이드: 정확한 페이로드 위치와 크기만큼 지정하여 QImage 생성
        QImage image = QImage::fromData(
            reinterpret_cast<const uchar*>(buffer.constData()) + sizeof(header),
            header.payload_len,
            "JPEG"
            );

        // 6) 🌟 [시연 어필 포인트] 명세서 제안: 지연 시간(Latency) 모니터링
        qint64 current_time = QDateTime::currentMSecsSinceEpoch();
        qint64 latency = current_time - header.timestamp_ms;
        qDebug() << "Channel:" << header.channel << " | Latency:" << latency << "ms";

        // 7) 사용이 끝난 패킷만큼 버퍼 맨 앞에서 깔끔하게 도려내기
        buffer.remove(0, total);

        // 8) 🌟 명세서 가이드: channel 값으로 4분할 위젯 분배 및 렌더링
        if (!image.isNull()) {
            if (header.channel >= 0 && header.channel < 4) {
                QPixmap pixmap = QPixmap::fromImage(image);
                QLabel* targetLabel = channelLabels[header.channel];

                // 비율 유지하며 화면에 가득 차게 뿌리기
                targetLabel->setPixmap(pixmap.scaled(targetLabel->size(),
                                                     Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation));
            }
        }
    }
}