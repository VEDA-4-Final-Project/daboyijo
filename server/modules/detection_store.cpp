#include "detection_store.hpp"

#include <cmath>
#include <limits>
#include <utility>

void DetectionStore::push(int channel, std::vector<Detection> dets,
                          std::chrono::steady_clock::time_point captured_at) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = history_[channel];
    // 저장 시각은 PTS 기반 촬영 시각(captured_at) — 영상 프레임과 같은 타임라인.
    history.push_back({captured_at, std::move(dets)});

    // "최근 history_sec_초 분량"만 유지 — 시간 기준 삭제 (정리 기준은 실시간 now())
    const auto boundary =
        now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(history_sec_));
    while (!history.empty() && history.front().timestamp < boundary) {
        history.pop_front();
    }
}

std::vector<Detection> DetectionStore::closestTo(
    int channel, std::chrono::steady_clock::time_point t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(channel);
    if (it == history_.end() || it->second.empty()) return {};

    const auto& history = it->second;
    auto best_it = history.begin();
    double min_diff = std::numeric_limits<double>::max();
    for (auto h = history.begin(); h != history.end(); ++h) {
        // 영상의 생성 시각과 좌표의 생성 시각 차이 (초)
        double diff =
            std::abs(std::chrono::duration<double>(h->timestamp - t).count());
        if (diff < min_diff) {
            min_diff = diff;
            best_it = h;
        }
    }
    return best_it->detections;
}

std::vector<Detection> DetectionStore::latest(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(channel);
    if (it == history_.end() || it->second.empty()) return {};
    return it->second.back().detections;
}
