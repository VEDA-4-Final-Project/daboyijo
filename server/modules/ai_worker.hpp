#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "detection.hpp"

// AI 스레드에 넘길 일감 1건.
struct AiJob {
    // 마스킹 전 깨끗한 960x540 프레임 — MoveNet 크롭과 보호사 색상 감지 공용.
    cv::Mat frame;
    int channel = -1;
    std::vector<Detection> dets;  // 이 프레임과 시간 매칭된 감지 좌표
};

// [공용 인프라] 무거운 AI 연산(보호사 색 판정, MoveNet 자세 추정) 전담 스레드.
//
// 메인 스트리밍 루프는 submit()으로 채널별 최신 프레임 1장씩만 넘기고
// (덮어쓰기 방식 — 일감이 밀려도 스트리밍은 방해받지 않음), 워커 스레드가
// 등록된 프로세서들을 등록 순서대로 실행한다.
//
// 새 AI 기능을 붙일 땐 자기 모듈에서 addProcessor()로 등록만 하면 된다
// — 이 파일은 수정할 일이 없다.
class AiWorker {
public:
    using Processor = std::function<void(const AiJob&)>;

    // start() 전에 등록할 것 (스레드 시작 후 등록은 미지원)
    void addProcessor(Processor p);

    // 채널별 최신 일감 1장만 유지 (덮어쓰기로 밀림 방지). 아무 스레드나 호출 가능.
    void submit(AiJob job);

    void start();
    void stop();  // 스레드 join까지 수행

private:
    void run();

    std::vector<Processor> processors_;
    std::mutex mutex_;
    std::map<int, AiJob> pending_;  // 채널별 최신 일감 1장씩만 보관
    std::thread thread_;
    std::atomic<bool> running_{false};
};
