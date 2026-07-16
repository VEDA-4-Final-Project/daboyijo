#include "ai_worker.hpp"

#include <chrono>
#include <utility>

void AiWorker::addProcessor(Processor p) {
    processors_.push_back(std::move(p));
}

void AiWorker::submit(AiJob job) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_[job.channel] = std::move(job);
}

void AiWorker::start() {
    running_ = true;
    thread_ = std::thread(&AiWorker::run, this);
}

void AiWorker::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AiWorker::run() {
    while (running_) {
        AiJob job;
        bool got_job = false;

        // 일감 바구니에 새 영상이 있는지 확인하고 꺼내오기
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()) {
                auto it = pending_.begin();
                job = std::move(it->second);
                pending_.erase(it);
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
