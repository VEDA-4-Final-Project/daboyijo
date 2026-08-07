#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// blackbox_clips 디렉토리를 정적 파일로 서빙하는 초경량 HTTP 서버.
// GET /<파일명> 요청만 처리한다 — Qt 클라이언트의 QMediaPlayer가
// http://<서버IP>:<포트>/<파일명> 으로 블랙박스 mp4를 바로 재생할 수 있게 해준다.
// Range(부분) 요청을 지원한다 — FFmpeg 기반 플레이어의 재생바 탐색(seek)이
// 파일 중간부터 다시 받는 Range 요청으로 동작하기 때문에 필수.
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
    // accept 루프와 stop()이 동시에 만지므로 원자적으로 다룬다.
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    // 요청 스레드는 detach라 join으로 기다릴 수 없다. 대신 처리 중인 개수를 세어
    // stop()이 그게 0이 될 때까지 기다린다 — 안 기다리면 전송 중인 스레드가
    // 이미 파괴된 이 객체의 멤버(rootDir_ 등)를 읽는다.
    int inflight_ = 0;
    std::mutex inflight_mutex_;
    std::condition_variable inflight_cv_;
};
