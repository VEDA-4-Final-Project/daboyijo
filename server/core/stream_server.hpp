#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
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
    // Qt에서 보낸 ROI 갱신 1건. points는 화면 대비 0~1 정규화 다각형.
    // clear=true면 해당 채널 ROI 삭제(이때 points는 비어 있음).
    struct RoiUpdate {
        int channel = 0;
        bool clear = false;
        std::vector<std::pair<float, float>> points;  // (x,y) 0~1
    };
    using RoiCallback = std::function<void(const RoiUpdate&)>;

    explicit StreamServer(int port);
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // ROI 수신 콜백. start() 전에 등록할 것 (접속 즉시 수신 스레드가 뜬다).
    void setRoiCallback(RoiCallback cb) { on_roi_ = std::move(cb); }

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
        std::thread receiver;  // 클라→서버 제어 메시지(ROI) 수신
        std::atomic<bool> alive{true};
    };

    void acceptLoop();
    void senderLoop(Client& client);
    void receiverLoop(Client& client);  // 제어 메시지 파싱 → on_roi_
    void closeClient(Client& client);   // alive=false → 소켓 셧다운 → 스레드 join → close

    const int port_;
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    RoiCallback on_roi_;
};
