#include "ai_worker.hpp"

#include <chrono>
#include <utility>

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
    while (running_) {
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

        for (auto& process : processors_) {
            process(job);
        }
    }
}
