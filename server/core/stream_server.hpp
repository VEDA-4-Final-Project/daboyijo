#pragma once

#include <openssl/ssl.h>

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

// 처리된 JPEG 프레임을 관제 클라이언트(Qt)로 송출하는 TCP 서버
// 패킷 형식은 protocol/video_stream.h 참조.
// TLS: cert_path/key_path를 둘 다 주면 접속마다 OpenSSL로 핸드셰이크(TLS 서버,
// 클라 인증서는 요구 안 함 — Qt가 서버만 검증). 비우면 평문(기존 동작).
//
// 클라이언트마다 전송 스레드와 대기열(outbox)을 따로 둬서 느린 클라이언트가
// 파이프라인·다른 클라이언트에 영향을 안 줌 (차면 오래된 프레임부터 버림)
class StreamServer {
public:
    // Qt 가 보낸 침대 ROI 갱신 1건 — points 는 화면 대비 0~1 정규화 다각형.
    // 한 채널에 침대가 여럿이라 roi_id 로 어느 침대인지 구분한다.
    // clear=true 면 그 침대 ROI 삭제 (points 는 빈 상태),
    // clear 이면서 roi_id==DBJ_ROI_ID_ALL 이면 그 채널 전부 삭제.
    struct RoiUpdate {
        int channel = 0;
        int roi_id = 0;
        bool clear = false;
        std::vector<std::pair<float, float>> points;  // (x,y) 0~1
    };
    using RoiCallback = std::function<void(const RoiUpdate&)>;
    // 침대 ↔ 입소자 매핑 — (채널, 침대, resident_id). resident_id=0 이면 해제
    using RoiBindCallback =
        std::function<void(int channel, int roi_id, int resident_id)>;

    using ConfirmCallback = std::function<void(int channel)>;
    // 위험도 변경 콜백 — (채널, 침대, 위험도 레벨).
    // 위험도는 사람에게 붙는 값이라 침대 단위다. roi_id==DBJ_ROI_ID_ALL 이면 채널 일괄.
    using RiskLevelCallback =
        std::function<void(int channel, int roi_id, int risk_level)>;
    // Qt 의 카메라 지정/해제 — url 은 채널의 전체 RTSP 주소
    // ★ 아래 콜백들은 전부 수신 스레드에서 불림 — 스레드 안전 + 블로킹 금지
    using CameraSetCallback = std::function<void(int channel, const std::string& url)>;
    using CameraClearCallback = std::function<void(int channel)>;
    // 이미지 파라미터 조절 (밝기/대비/채도, 각 0~100) — ONVIF HTTP 는 별도 스레드로
    using ImageSetCallback =
        std::function<void(int channel, int brightness, int contrast, int saturation)>;
    // 초점 조절 — area=false 면 전체 자동초점(nx,ny 무시), true 면 클릭 지점 영역 초점
    // SUNAPI HTTP 도 별도 스레드로
    using FocusCallback =
        std::function<void(int channel, bool area, float nx, float ny)>;
    // [영상검색] Qt 관제 화면의 자연어 질의. clientId는 sendSearchResult로 그
    // 클라이언트에게만 회신할 때 쓴다(다른 관제 PC로 새지 않게). Gemini+DB 왕복이
    // 있으니 호출부는 반드시 detached 스레드에서 처리할 것(다른 콜백과 동일 원칙).
    using SearchQueryCallback =
        std::function<void(int channel, uint64_t clientId, const std::string& query)>;

    // cert_path/key_path를 둘 다 넘기면 TLS로 뜬다. 파일을 못 읽거나 키가 인증서와
    // 안 맞으면 start()가 false를 반환한다(잘못된 설정으로 조용히 평문으로 내려가는
    // 것을 막기 위해 — 절반만 넘기면(한쪽만 빈 값) 즉시 구성 오류로 취급한다).
    explicit StreamServer(int port, std::string cert_path = "",
                          std::string key_path = "");
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // ★ 콜백 등록은 전부 start() 전에 — 접속 즉시 수신 스레드가 뜸
    void setRoiCallback(RoiCallback cb) { on_roi_ = std::move(cb); }
    void setRoiBindCallback(RoiBindCallback cb) { on_roi_bind_ = std::move(cb); }
    void setConfirmCallback(ConfirmCallback cb) { on_confirm_ = std::move(cb); }
    void setRiskLevelCallback(RiskLevelCallback cb) { on_risk_level_ = std::move(cb); }
    void setCameraSetCallback(CameraSetCallback cb) { on_camera_set_ = std::move(cb); }
    void setCameraClearCallback(CameraClearCallback cb) { on_camera_clear_ = std::move(cb); }
    void setImageSetCallback(ImageSetCallback cb) { on_image_set_ = std::move(cb); }
    void setFocusCallback(FocusCallback cb) { on_focus_ = std::move(cb); }
    void setSearchQueryCallback(SearchQueryCallback cb) { on_search_query_ = std::move(cb); }

    bool start();
    void stop();

    // 접속한 모든 클라이언트에 채널 프레임 1장 전송
    void broadcast(int channel, std::vector<unsigned char> jpeg);

    // 접속한 모든 클라이언트에 이벤트(dbj_evt_header_t) 전송
    // type 은 DBJ_EVT_*, (x,y)는 발생 위치 정규화 0~1 (없으면 0,0)
    // roi_id 는 발생 침대(=누구인지) — 특정 못 하면 DBJ_ROI_ID_NONE 을 넘길 것.
    // timestampMsOverride 가 0 이 아니면 그 값을 그대로 사용 — 블랙박스 클립
    // 파일명과 시각을 맞춰야 할 때 씀 (0 이면 서버 현재 시각)
    // 아무 스레드에서나 호출 가능 (낙상 콜백은 AI 워커 스레드)
    void broadcastEvent(int channel, uint8_t type, float x, float y,
                        int roi_id, int64_t timestampMsOverride = 0);

    // [영상검색] 특정 클라이언트에게만 검색 결과 회신 (SearchQueryCallback이 받은
    // clientId 그대로). 그 사이 클라이언트가 끊겼으면 조용히 버림 — 재연결한
    // 클라를 잘못 짚느니 응답을 잃는 편이 안전하다(clientId는 접속마다 새로 발급).
    void sendSearchResult(uint64_t clientId, int channel, const std::string& text);

    size_t clientCount();

private:
    using Packet = std::shared_ptr<const std::vector<unsigned char>>;

    struct Client {
        int fd = -1;
        SSL* ssl = nullptr;    // tls_enabled_ 일 때만 사용, 아니면 nullptr
        uint64_t id = 0;       // 접속마다 고유(fd는 재사용될 수 있어 못 씀) — sendSearchResult 대상 식별
        std::deque<Packet> outbox;
        std::mutex mutex;
        std::condition_variable cv;
        std::thread sender;
        std::thread receiver;  // 클라 → 서버 제어 메시지 수신
        std::atomic<bool> alive{true};
    };

    bool initTls();  // ssl_ctx_ 구성 — start()에서 cert/key가 있을 때만 호출
    // 두 함수는 tls_enabled_ 여부에 따라 SSL_read/write 또는 recv/send로 갈라짐 —
    // 이 두 곳 말고는 accept 루프의 SSL_accept 핸드셰이크가 유일한 TLS 분기점.
    ssize_t clientRecv(Client& client, void* buf, size_t len);
    ssize_t clientSend(Client& client, const void* buf, size_t len);
    void acceptLoop();
    void enqueueAll(Packet packet);     // 모든 outbox 에 적재 + 죽은 클라 정리
    void senderLoop(Client& client);
    void receiverLoop(Client& client);  // 제어 메시지 파싱 → 각 콜백
    void closeClient(Client& client);   // alive=false → 셧다운 → join → close

    const int port_;
    const std::string cert_path_;
    const std::string key_path_;
    bool tls_enabled_ = false;
    SSL_CTX* ssl_ctx_ = nullptr;
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_client_id_{1};  // 0은 "대상 없음"으로 남겨둠

    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;

    RoiCallback on_roi_;
    RoiBindCallback on_roi_bind_;
    ConfirmCallback on_confirm_;
    RiskLevelCallback on_risk_level_;
    CameraSetCallback on_camera_set_;
    CameraClearCallback on_camera_clear_;
    ImageSetCallback on_image_set_;
    FocusCallback on_focus_;
    SearchQueryCallback on_search_query_;
};
