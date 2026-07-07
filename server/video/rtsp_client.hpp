#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "frame_queue.hpp"

// RTSP 채널 1개의 수신·디코딩을 담당하는 워커.
// 연결이 끊기면 백오프 후 자동 재연결한다.
class RtspClient {
public:
    RtspClient(int channel, std::string url, FrameQueue& queue);
    ~RtspClient();

    RtspClient(const RtspClient&) = delete;
    RtspClient& operator=(const RtspClient&) = delete;

    void start();
    void stop();

    int channel() const { return channel_; }
    bool connected() const { return connected_.load(); }
    uint64_t frameCount() const { return frame_count_.load(); }

private:
    void run();

    const int channel_;
    const std::string url_;
    FrameQueue& queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> frame_count_{0};
};
