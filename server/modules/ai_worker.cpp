#include "ai_worker.hpp"

#include <chrono>
#include <utility>

namespace {

// 채널당 AI 처리 주기 하한 — 파이프라인은 15fps로 일감을 던지지만 전부 처리할
// 필요가 없다 (요양사 색 판정은 세션 타이머 해상도상 초당 몇 번이면 충분하고,
// 자세 추정은 fall_module이 객체당 2초 주기로 따로 제한). 단일 워커 시절엔
// 스레드 1개가 자연스럽게 총량을 눌렀지만, 채널별 워커에선 이 값이 CPU 상한
// 조절 손잡이다. 값을 줄이면 반응이 빨라지고 CPU가 오른다.
constexpr double kMinJobIntervalSec = 0.25;  // 채널당 최대 4회/초

}  // namespace

void AiWorker::addProcessor(Processor p) {
    processors_.push_back(std::move(p));
}

void AiWorker::addChannel(int channel) {
    if (slots_.count(channel)) return;  // 이미 등록됨
    slots_[channel] = std::make_unique<Slot>();
}

void AiWorker::submit(AiJob job) {
    auto it = slots_.find(job.channel);
    if (it == slots_.end()) return;  // 미등록 채널 — 버림

    Slot& slot = *it->second;
    std::lock_guard<std::mutex> lock(slot.mutex);
    slot.pending = std::move(job);
    slot.has_job = true;
}

void AiWorker::start() {
    running_ = true;
    for (auto& entry : slots_) {
        Slot& slot = *entry.second;
        slot.thread = std::thread(&AiWorker::run, this, std::ref(slot));
    }
}

void AiWorker::stop() {
    running_ = false;
    for (auto& entry : slots_) {
        Slot& slot = *entry.second;
        if (slot.thread.joinable()) slot.thread.join();
    }
}

void AiWorker::run(Slot& slot) {
    auto last_run = std::chrono::steady_clock::now() -
                    std::chrono::seconds(1);  // 첫 일감은 바로 처리

    while (running_) {
        // 처리 주기 하한 유지 — 일감함엔 최신 1장만 남으니 밀릴 걱정 없음
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          last_run)
                .count() < kMinJobIntervalSec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AiJob job;
        bool got_job = false;

        // 내 채널 일감함에 새 영상이 있는지 확인하고 꺼내오기
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            if (slot.has_job) {
                job = std::move(slot.pending);
                slot.has_job = false;
                got_job = true;
            }
        }

        // 일감이 없으면 CPU를 쉬게 해준다 (10ms 대기)
        if (!got_job) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        last_run = std::chrono::steady_clock::now();
        for (auto& process : processors_) {
            process(job);
        }
    }
}
