#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "detection.hpp"
#include "frame_queue.hpp"

// libav(FFmpeg) 직접 기반 RTSP 수신 워커.
// OpenCV VideoCapture와 달리 영상 트랙 + ONVIF 메타데이터(data) 트랙을
// 같은 연결에서 함께 받는다.
//   - 영상 패킷 → H.264 디코딩 → cv::Mat → FrameQueue (기존과 동일)
//   - data 패킷 → XML → MetadataParser → onDetections 콜백
//
// 연결이 끊기면 백오프 후 자동 재연결.
class RtspAvClient {
public:
    // 감지 결과 콜백. 메타데이터 프레임마다 호출된다(사람 없으면 빈 벡터).
    using DetectionCallback =
        std::function<void(int channel, std::vector<Detection>)>;

    RtspAvClient(int channel, std::string url, FrameQueue& queue);
    ~RtspAvClient();

    RtspAvClient(const RtspAvClient&) = delete;
    RtspAvClient& operator=(const RtspAvClient&) = delete;

    // 메타데이터 감지 결과를 받을 콜백 등록 (start 전에 설정)
    void setDetectionCallback(DetectionCallback cb) { on_detections_ = std::move(cb); }

    void start();
    void stop();

    int channel() const { return channel_; }
    bool connected() const { return connected_.load(); }
    uint64_t frameCount() const { return frame_count_.load(); }
    uint64_t metadataCount() const { return metadata_count_.load(); }

private:
    void run();
    bool openAndStream();  // 1회 연결·수신 루프. 끊기면 false 반환

    const int channel_;
    const std::string url_;
    FrameQueue& queue_;
    DetectionCallback on_detections_;
    std::string meta_buf_;  // 메타데이터 XML 조각 재조립 버퍼 (수신 스레드 전용)

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> metadata_count_{0};
};
