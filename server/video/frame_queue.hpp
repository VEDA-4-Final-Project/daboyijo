#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

#include <opencv2/core.hpp>

// 화면 송출용 축소 해상도 — 관제 Qt 그리드 타일 크기에 맞춘 값(FHD 2×2면 타일당
// 딱 960×540 1:1). AI(MoveNet)는 image 원본을 따로 받으므로 이 값은 순수 '보이는
// 영상' 크기다. video_pipeline.cpp의 kViewSize는 반드시 이 값과 일치시킬 것.
constexpr int kViewWidth = 960;
constexpr int kViewHeight = 540;

// 디코딩된 1프레임. 채널 번호와 수신 시각을 함께 보관한다.
struct Frame {
    int channel;
    cv::Mat image;  // 원본 해상도 BGR (AI 크롭용 — 다운스케일 금지)
    std::chrono::steady_clock::time_point received_at;
    // 화면 송출용 축소본(kViewWidth×kViewHeight). 디코딩 스레드가 sws_scale로
    // 변환과 동시에 미리 만들어 둔다 → 파이프라인이 cv::resize 없이 바로 송출.
    // 비어 있으면(구형 push 경로 등) 파이프라인이 image에서 resize하는 폴백 동작.
    cv::Mat view;
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
