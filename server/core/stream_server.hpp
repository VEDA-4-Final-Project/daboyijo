#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// 처리된 JPEG 프레임을 관제 클라이언트(Qt)로 송출하는 TCP 서버.
// 패킷 형식: protocol/video_stream.h 참조.
// v1은 평문 TCP — 파이프라인 검증용. 검증 후 OpenSSL TLS로 감싼다.
//
// 클라이언트마다 전송 스레드와 대기열(outbox)을 따로 두어,
// 느린 클라이언트가 있어도 파이프라인과 다른 클라이언트에 영향을 주지 않는다
// (대기열이 차면 그 클라이언트의 오래된 프레임부터 버림).
class StreamServer {
public:
    explicit StreamServer(int port);
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    bool start();
    void stop();

    // 접속한 모든 클라이언트에 채널 프레임 1장 전송
    void broadcast(int channel, std::vector<unsigned char> jpeg);

    size_t clientCount();

private:
    using Packet = std::shared_ptr<const std::vector<unsigned char>>;

    struct Client {
        int fd = -1;
        std::deque<Packet> outbox;
        std::mutex mutex;
        std::condition_variable cv;
        std::thread sender;
        std::atomic<bool> alive{true};
    };

    void acceptLoop();
    void senderLoop(Client& client);

    const int port_;
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
};
