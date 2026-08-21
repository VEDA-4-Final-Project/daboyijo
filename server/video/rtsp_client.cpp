#include "rtsp_client.hpp"

#include <chrono>
#include <cstdio>

#include <opencv2/videoio.hpp>

namespace {
constexpr int kReconnectDelaySec = 3;  // 재연결 백오프
}

RtspClient::RtspClient(int channel, std::string url, FrameQueue& queue)
    : channel_(channel), url_(std::move(url)), queue_(queue) {}

RtspClient::~RtspClient() {
    stop();
}

void RtspClient::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&RtspClient::run, this);
}

void RtspClient::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RtspClient::run() {
    while (running_.load()) {
        // FFmpeg 백엔드로 RTSP 접속 (RPi에서는 TCP 전송이 안정적:
        // 환경변수 OPENCV_FFMPEG_CAPTURE_OPTIONS="rtsp_transport;tcp")
        cv::VideoCapture cap(url_, cv::CAP_FFMPEG);
        if (!cap.isOpened()) {
            std::fprintf(stderr, "[ch%d] RTSP 연결 실패, %d초 후 재시도\n",
                         channel_, kReconnectDelaySec);
            for (int i = 0; i < kReconnectDelaySec * 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        connected_.store(true);
        std::fprintf(stderr, "[ch%d] RTSP 연결됨: %.0fx%.0f\n", channel_,
                     cap.get(cv::CAP_PROP_FRAME_WIDTH),
                     cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        cv::Mat image;
        while (running_.load() && cap.read(image)) {
            frame_count_.fetch_add(1);
            // view는 비워 둔다 — 이 경로(OpenCV VideoCapture)는 축소본을 미리 만들지
            // 않으므로 파이프라인이 image로 폴백 축소(cv::resize)한다.
            queue_.push(Frame{channel_, image.clone(),
                              std::chrono::steady_clock::now(), cv::Mat{}});
        }

        connected_.store(false);
        if (running_.load()) {
            std::fprintf(stderr, "[ch%d] 스트림 끊김, 재연결 시도\n", channel_);
        }
    }
}
