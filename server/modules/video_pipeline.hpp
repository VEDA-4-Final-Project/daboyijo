#pragma once

#include <csignal>
#include <functional>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "ai_worker.hpp"
#include "detection.hpp"
#include "detection_store.hpp"
#include "frame_queue.hpp"
#include "stats_reporter.hpp"
#include "stream_server.hpp"

// [공용 인프라] 영상 스트리밍 메인 루프.
//
// 프레임 큐에서 꺼내 → (15fps 제한) → 감지 좌표 시간 매칭 → 등록된 프레임
// 스테이지(블러 등) 순서대로 적용 → AI 워커에 깨끗한 프레임 전달 →
// JPEG 인코딩 → Qt 송출 → 통계 갱신.
//
// 기능 추가는 addStage()(송출 영상 가공) 또는 AiWorker.addProcessor()(무거운
// 분석)로 — 이 파일은 수정할 일이 없다.
class VideoPipeline {
public:
    // 송출 전 프레임 가공 단계. image를 제자리 수정한다 (예: 블러 마스킹).
    using FrameStage = std::function<void(
        int channel, cv::Mat& image, const std::vector<Detection>& dets)>;

    VideoPipeline(FrameQueue& queue, StreamServer& server,
                  DetectionStore& store, AiWorker& ai, StatsReporter& stats)
        : queue_(queue), server_(server), store_(store), ai_(ai),
          stats_(stats) {}

    // run() 전에 등록할 것. 실행 순서 = 등록 순서.
    void addStage(FrameStage s) { stages_.push_back(std::move(s)); }

    // 메인 루프 — stop이 1이 될 때까지 블로킹 (시그널 핸들러가 세팅)
    void run(const volatile std::sig_atomic_t& stop);

private:
    FrameQueue& queue_;
    StreamServer& server_;
    DetectionStore& store_;
    AiWorker& ai_;
    StatsReporter& stats_;
    std::vector<FrameStage> stages_;
};
