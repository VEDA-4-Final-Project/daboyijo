#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

    using ConfirmCallback = std::function<void(int channel)>;
    // 위험도 변경 시 호출될 콜백 함수 타입 정의 (채널 번호, 위험도 레벨)
    using RiskLevelCallback = std::function<void(int channel, int risk_level)>;
    // Qt가 카메라를 지정/해제할 때 호출 — url은 채널의 전체 RTSP 주소.
    // 수신 스레드에서 호출되므로 콜백 구현은 스레드 안전해야 한다.
    using CameraSetCallback = std::function<void(int channel, const std::string& url)>;
    using CameraClearCallback = std::function<void(int channel)>;

    explicit StreamServer(int port);
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // ROI 수신 콜백. start() 전에 등록할 것 (접속 즉시 수신 스레드가 뜬다).
    void setRoiCallback(RoiCallback cb) { on_roi_ = std::move(cb); }
    // 경보 확인 수신 콜백. start() 전 등록
    void setConfirmCallback(ConfirmCallback cb) { on_confirm_ = std::move(cb); }
    // 위험도 수신 콜백. start() 전 등록
    void setRiskLevelCallback(RiskLevelCallback cb) { on_risk_level_ = std::move(cb); }
    // 카메라 연결/해제 수신 콜백. start() 전 등록
    void setCameraSetCallback(CameraSetCallback cb) { on_camera_set_ = std::move(cb); }
    void setCameraClearCallback(CameraClearCallback cb) { on_camera_clear_ = std::move(cb); }

    bool start();
    void stop();

    // 접속한 모든 클라이언트에 채널 프레임 1장 전송
    void broadcast(int channel, std::vector<unsigned char> jpeg);

    // 접속한 모든 클라이언트에 이벤트(dbj_evt_header_t) 전송.
    // type은 DBJ_EVT_*, (x,y)는 발생 위치 정규화 0~1 (없으면 0,0).
    // timestampMsOverride를 0이 아닌 값으로 주면 그 값을 그대로 timestamp_ms에
    // 싣는다(기본 0이면 지금까지처럼 서버 현재 시각을 사용). 블랙박스 클립
    // 파일명과 같은 시각을 써야 하는 등, 호출자가 이벤트 시각을 다른 값과
    // 맞춰야 할 때 쓴다.
    // 아무 스레드에서나 호출 가능 (낙상 콜백은 AI 워커 스레드에서 온다).
    void broadcastEvent(int channel, uint8_t type, float x, float y,
                        int64_t timestampMsOverride = 0);

    // 관절 1개 — 프레임 전체 기준 정규화 0~1 좌표 + 신뢰도 0~1.
    struct PosePoint {
        float x = 0, y = 0, score = 0;
    };
    // 접속한 모든 클라이언트에 스켈레톤(자세) 1건 전송 (dbj_pose_header_t + 점들).
    // 낙상감지 MoveNet 결과를 Qt에서 시각화하기 위한 오버레이 보조 데이터.
    // 오래된 프레임처럼 드롭 가능(이벤트와 달리 outbox가 차면 버려질 수 있음).
    // 아무 스레드에서나 호출 가능 (AI 워커 스레드에서 온다).
    void broadcastPose(int channel, int object_id,
                       const std::vector<PosePoint>& points);

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
    void enqueueAll(Packet packet);  // 모든 클라이언트 outbox에 적재 (죽은 클라 정리 겸)
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
    ConfirmCallback on_confirm_;
    RiskLevelCallback on_risk_level_;
    CameraSetCallback on_camera_set_;
    CameraClearCallback on_camera_clear_;
};
