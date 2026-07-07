#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

#include <opencv2/core.hpp>

// 디코딩된 1프레임. 채널 번호와 수신 시각을 함께 보관한다.
struct Frame {
    int channel;
    cv::Mat image;
    std::chrono::steady_clock::time_point received_at;
};

// 수신 스레드(RtspClient) → 처리 스레드 간 프레임 전달용 큐.
// 실시간 모니터링이 목적이므로 가득 차면 가장 오래된 프레임을 버린다.
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 8) : capacity_(capacity) {}

    void push(Frame frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
            }
            queue_.push_back(std::move(frame));
        }
        cv_.notify_one();
    }

    // timeout 내에 프레임이 없으면 nullopt 반환
    std::optional<Frame> pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }
        Frame frame = std::move(queue_.front());
        queue_.pop_front();
        return frame;
    }

private:
    size_t capacity_;
    std::deque<Frame> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
