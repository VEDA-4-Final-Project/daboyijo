#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// blackbox_clips 를 정적 서빙하는 초경량 HTTP 서버 — GET /<파일명> 만 처리
// Qt 의 QMediaPlayer 가 http://<서버IP>:<포트>/<파일명> 으로 mp4 를 바로 재생
// Range 요청 지원 필수 — FFmpeg 계열의 재생바 탐색이 Range 로 동작함
// 인증 없음, 사내망 전용 — 외부 노출 시 인증·HTTPS 필요
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
    // accept 루프와 stop() 이 동시에 만져서 원자적으로 다룸
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    // 요청 스레드가 detach 라 join 을 못 함 — 처리 중인 개수를 세어 stop() 이 0 을 기다림
    // 안 기다리면 전송 중인 스레드가 파괴된 멤버(rootDir_ 등)를 읽음
    int inflight_ = 0;
    std::mutex inflight_mutex_;
    std::condition_variable inflight_cv_;
};
