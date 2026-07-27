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
#include "snapshot_buffer.hpp"
#include "stats_reporter.hpp"
#include "stream_server.hpp"

// [공용 인프라] 영상 스트리밍 메인 루프.
//
// 프레임 큐에서 꺼내 → (15fps 제한) → 감지 좌표 시간 매칭 → 등록된 프레임
// 스테이지(블러 등) 순서대로 적용 → AI 워커에 깨끗한 프레임 전달 →
// JPEG 인코딩 → Qt 송출 → 통계 갱신.
//
// 스테이지 적용 후의 프레임은 "전원 블러본" — 스냅샷 버퍼 A(snapshots_)에
// 저장돼 Gemini/외부로 나간다. 낙상 중에는 setFallVariant()로 등록한 콜백이
// 이 전원 블러본에서 "낙상자만 노출한 선택본"을 추가로 만들어, 관제 Qt 송출과
// 스냅샷 버퍼 B(snapshots_fall_, 보호자 텔레그램용)에 쓴다. 낙상이 아니면
// 전원 블러본을 그대로 송출한다.
//
// 기능 추가는 addStage()(송출 영상 가공) 또는 AiWorker.addProcessor()(무거운
// 분석)로.
class VideoPipeline {
public:
    // 송출 전 프레임 가공 단계. image를 제자리 수정한다 (예: 블러 마스킹).
    using FrameStage = std::function<void(
        int channel, cv::Mat& image, const std::vector<Detection>& dets)>;

    // 낙상 선택본 생성 콜백. full_blur(전원 블러본)와 clean(블러 전 원본)을 받아
    // 낙상자만 노출한 선택본을 out에 채우고 true를 반환한다. 낙상이 아니거나
    // 낙상자를 특정 못 하면 false(=전원 블러본 그대로 송출).
    using FallVariant = std::function<bool(
        int channel, const cv::Mat& full_blur, const cv::Mat& clean,
        const std::vector<Detection>& dets, cv::Mat& out)>;

    VideoPipeline(FrameQueue& queue, StreamServer& server,
                  DetectionStore& store, AiWorker& ai, StatsReporter& stats,
                  SnapshotBuffer& snapshots, SnapshotBuffer& snapshots_fall)
        : queue_(queue), server_(server), store_(store), ai_(ai),
          stats_(stats), snapshots_(snapshots), snapshots_fall_(snapshots_fall) {}

    // run() 전에 등록할 것. 실행 순서 = 등록 순서.
    void addStage(FrameStage s) { stages_.push_back(std::move(s)); }

    // 낙상 선택본 생성 콜백 등록 (선택 사항). 미등록이면 항상 전원 블러본 송출.
    void setFallVariant(FallVariant f) { fall_variant_ = std::move(f); }

    // 메인 루프 — stop이 1이 될 때까지 블로킹 (시그널 핸들러가 세팅)
    void run(const volatile std::sig_atomic_t& stop);

private:
    FrameQueue& queue_;
    StreamServer& server_;
    DetectionStore& store_;
    AiWorker& ai_;
    StatsReporter& stats_;
    SnapshotBuffer& snapshots_;       // 버퍼 A: 전원 블러본 (Gemini/외부용)
    SnapshotBuffer& snapshots_fall_;  // 버퍼 B: 낙상 선택본 (보호자 텔레그램용)
    std::vector<FrameStage> stages_;
    FallVariant fall_variant_;
};
