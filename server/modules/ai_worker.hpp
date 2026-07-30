#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "detection.hpp"

// AI 스레드에 넘길 일감 1건. 두 이미지 모두 마스킹 전 깨끗한 프레임이다.
struct AiJob {
    cv::Mat raw_frame;    // MoveNet 크롭용 원본 해상도 이미지 — 다운스케일하면
                          // 원거리 사람의 자세 감지가 불안정해짐 (rtsp_av_client.cpp
                          // 상단 실험 기록 참조)
    cv::Mat small_frame;  // 보호사 색상 감지용 축소(960x540) 이미지
    int channel = -1;
    std::vector<Detection> dets;  // 이 프레임과 시간 매칭된 감지 좌표
};

// [공용 인프라] 무거운 AI 연산(보호사 색 판정, MoveNet 자세 추정) 전담 스레드.
//
// ★ 채널별 전담 워커 구조 — 채널마다 스레드 1개씩 배정해 한 채널의 추론이
//   느려도 다른 채널의 분석이 밀리지 않는다. 같은 채널의 일감은 항상 같은
//   스레드가 처리하므로, 프로세서 안의 "채널별 상태"는 락 없이 써도 된다
//   (단, 채널 간 공유 상태는 각 모듈이 직접 보호할 것).
//
// 메인 스트리밍 루프는 submit()으로 채널별 최신 프레임 1장씩만 넘기고
// (덮어쓰기 방식 — 일감이 밀려도 스트리밍은 방해받지 않음), 해당 채널의
// 워커 스레드가 등록된 프로세서들을 등록 순서대로 실행한다.
//
// 새 AI 기능을 붙일 땐 자기 모듈에서 addProcessor()로 등록만 하면 된다
// — 이 파일은 수정할 일이 없다.
class AiWorker {
public:
    using Processor = std::function<void(const AiJob&)>;

    // start() 전에 등록할 것 (스레드 시작 후 등록은 미지원)
    void addProcessor(Processor p);

    // 채널 등록 — 채널당 워커 스레드 1개가 생긴다. start() 전에 호출할 것.
    void addChannel(int channel);

    // 채널별 최신 일감 1장만 유지 (덮어쓰기로 밀림 방지). 아무 스레드나 호출 가능.
    // 등록 안 된 채널의 일감은 버린다.
    void submit(AiJob job);

    void start();
    void stop();  // 전체 워커 스레드 join까지 수행

private:
    // 채널 하나의 일감함 + 전담 스레드
    struct Slot {
        std::mutex mutex;
        AiJob pending;
        bool has_job = false;
        std::thread thread;
    };

    void run(Slot& slot);

    std::vector<Processor> processors_;
    std::map<int, std::unique_ptr<Slot>> slots_;  // 채널 → 전담 슬롯
    std::atomic<bool> running_{false};
};
