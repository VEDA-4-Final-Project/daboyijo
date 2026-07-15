#pragma once

#include <atomic>
#include <string>
#include <thread>

// blackbox_clips 디렉토리를 정적 파일로 서빙하는 초경량 HTTP 서버.
// GET /<파일명> 요청만 처리한다 — Qt 클라이언트의 QMediaPlayer가
// http://<서버IP>:<포트>/<파일명> 으로 블랙박스 mp4를 바로 재생할 수 있게 해준다.
// (인증 없음 — 사내망 전용 v1. 외부 노출 시 반드시 인증/HTTPS 추가할 것.)
class ClipHttpServer {
public:
    ClipHttpServer(int port, std::string rootDir);
    ~ClipHttpServer();

    ClipHttpServer(const ClipHttpServer&) = delete;
    ClipHttpServer& operator=(const ClipHttpServer&) = delete;

    bool start();
    void stop();

private:
    void acceptLoop();
    void handleClient(int fd);

    const int port_;
    const std::string rootDir_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};
