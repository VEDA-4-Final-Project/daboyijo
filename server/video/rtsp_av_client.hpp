#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "detection.hpp"
#include "frame_queue.hpp"

extern "C" {
struct AVPacket;
struct AVCodecParameters;
}

// libav(FFmpeg) 직접 기반 RTSP 수신 워커.
// OpenCV VideoCapture와 달리 영상 트랙 + ONVIF 메타데이터(data) 트랙을
// 같은 연결에서 함께 받는다.
//   - 영상 패킷 → H.264 디코딩 → cv::Mat → FrameQueue (기존과 동일)
//   - data 패킷 → XML → MetadataParser → onDetections 콜백
//
// 연결이 끊기면 백오프 후 자동 재연결.
//
// URL은 런타임에 바뀔 수 있다(Qt 관제 화면의 "카메라 연결"). 생성 시 URL이 비어
// 있으면 연결을 시도하지 않고 대기하다가, reconnect(url)이 오면 그 주소로 연결한다.
// disconnect()는 현재 스트림을 끊고 다시 대기 상태로 돌아간다. 진행 중인 av_read_frame
// /avformat_open_input은 AVIOInterruptCB로 즉시 중단시켜, URL 교체가 지연 없이 반영된다.
class RtspAvClient {
public:
    // 감지 결과 콜백. 메타데이터 프레임마다 호출된다(사람 없으면 빈 벡터).
    // captured_at: 메타 패킷 PTS로 환산한 촬영 시각(steady_clock 기준). 영상
    // 프레임의 PTS 촬영 시각과 같은 타임라인이라, 디코딩 지연과 무관하게 좌표를
    // 프레임에 매칭할 수 있다(PTS 없으면 수신 시각으로 폴백).
    using DetectionCallback =
        std::function<void(int channel, std::vector<Detection>,
                           std::chrono::steady_clock::time_point captured_at)>;

    // 압축 비디오 패킷 콜백(디코딩 전) — 블랙박스 녹화 등 원본 스트림이
    // 필요한 소비자용. 매 비디오 패킷마다 호출된다.
    using PacketCallback = std::function<void(const AVPacket* pkt)>;
    // 스트림 연결(재연결 포함) 성공 시 1회 호출 — 코덱 파라미터/타임베이스 전달.
    using StreamReadyCallback =
        std::function<void(const AVCodecParameters* codecpar, int timeBaseNum,
                           int timeBaseDen)>;

    RtspAvClient(int channel, std::string url, FrameQueue& queue);
    ~RtspAvClient();

    RtspAvClient(const RtspAvClient&) = delete;
    RtspAvClient& operator=(const RtspAvClient&) = delete;

    // 메타데이터 감지 결과를 받을 콜백 등록 (start 전에 설정)
    void setDetectionCallback(DetectionCallback cb) { on_detections_ = std::move(cb); }
    void setPacketCallback(PacketCallback cb) { on_packet_ = std::move(cb); }
    void setStreamReadyCallback(StreamReadyCallback cb) { on_stream_ready_ = std::move(cb); }

    void start();
    void stop();

    // 런타임 카메라 지정 — 이 URL로 (재)연결한다. 진행 중 스트림은 즉시 끊고 교체.
    // 아무 스레드에서나 호출 가능(스트림 서버 수신 스레드에서 온다).
    void reconnect(std::string url);
    // 현재 연결을 끊고 URL 없는 대기 상태로 되돌린다.
    void disconnect();

    int channel() const { return channel_; }
    bool connected() const { return connected_.load(); }
    uint64_t frameCount() const { return frame_count_.load(); }
    uint64_t metadataCount() const { return metadata_count_.load(); }

private:
    void run();
    bool openAndStream(const std::string& url);  // 1회 연결·수신 루프. 끊기면 false 반환
    // avformat 인터럽트 콜백 — 중단(정지/URL 교체) 요청 시 1을 반환해 블로킹 I/O를 깨운다.
    static int interruptCb(void* opaque);

    const int channel_;
    std::mutex url_mutex_;             // url_ 보호 (워커 스레드 읽기 / 제어 스레드 쓰기)
    std::condition_variable url_cv_;   // 대기 상태에서 URL 지정 시 워커를 깨움
    std::string url_;                  // 현재 대상 RTSP URL (비어 있으면 대기)
    std::atomic<bool> reload_{false};  // 진행 중 스트림을 끊고 URL을 다시 읽으라는 신호
    FrameQueue& queue_;
    DetectionCallback on_detections_;
    PacketCallback on_packet_;
    StreamReadyCallback on_stream_ready_;
    std::string meta_buf_;  // 메타데이터 XML 조각 재조립 버퍼 (수신 스레드 전용)

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> metadata_count_{0};
};
