#include "snapshot_buffer.hpp"

#include <algorithm>

void SnapshotBuffer::put(int channel, const Jpeg& jpeg,
                         std::chrono::steady_clock::time_point t) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dq = frames_[channel];
    dq.push_back({t, jpeg});

    // "최근 history_sec_초 분량"만 유지 — 시간 기준 삭제
    const auto boundary =
        t - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(history_sec_));
    while (!dq.empty() && dq.front().t < boundary) {
        dq.pop_front();
    }
}

std::vector<SnapshotBuffer::Jpeg> SnapshotBuffer::recentKeyframes(
    int channel, int n, double spanSec) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = frames_.find(channel);
    if (it == frames_.end() || it->second.empty() || n <= 0) return {};

    const auto& dq = it->second;
    const auto newest = dq.back().t;
    const auto window_start =
        newest - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(spanSec));

    // spanSec 구간 안의 프레임만 후보로 모은다 (오래된→최신 순 유지).
    std::vector<const Stamped*> window;
    for (const auto& s : dq) {
        if (s.t >= window_start) window.push_back(&s);
    }
    if (window.empty()) return {};

    // 후보에서 시간상 고르게 퍼지도록 최대 n장을 균등 인덱스로 뽑는다.
    std::vector<Jpeg> out;
    const int take = std::min<int>(n, static_cast<int>(window.size()));
    for (int i = 0; i < take; ++i) {
        // take==1이면 마지막(가장 최신) 1장. take>1이면 처음~끝을 균등 분할.
        size_t idx = (take == 1)
                         ? window.size() - 1
                         : static_cast<size_t>(
                               static_cast<double>(i) * (window.size() - 1) /
                               (take - 1));
        out.push_back(window[idx]->jpeg);
    }
    return out;
}
