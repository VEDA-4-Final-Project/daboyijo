#include "detection_store.hpp"

#include <algorithm>
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

    // 잔상 제거
    if (min_diff > 1.0) {
        return {}; 
    }

    return best_it->detections;
}

std::vector<Detection> DetectionStore::predictedAt(
    int channel, std::chrono::steady_clock::time_point t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(channel);
    if (it == history_.end() || it->second.empty()) return {};
    const auto& history = it->second;  // push_back만 하므로 시각 오름차순 정렬 보장

    // t 시점 바로 직전(<=t)의 최신 메타데이터 프레임(base)과, 속도 계산용으로
    // 그 한 프레임 전(prev)을 같이 찾는다.
    const TimestampedDets* base = nullptr;
    const TimestampedDets* prev = nullptr;
    for (const auto& h : history) {
        if (h.timestamp <= t) {
            prev = base;
            base = &h;
        } else {
            break;
        }
    }
    if (!base) base = &history.front();  // t가 이력보다 과거인 극초반 케이스 — 외삽 불가

    const double diff = std::chrono::duration<double>(t - base->timestamp).count();
    if (std::abs(diff) > 1.0) return {};  // 기존 closestTo()와 동일한 잔상 제거 기준

    if (!prev || diff <= 0.0) return base->detections;  // 속도 계산 불가 → 원본 그대로

    const double dt_hist =
        std::chrono::duration<double>(base->timestamp - prev->timestamp).count();
    if (dt_hist <= 0.0) return base->detections;

    // 메타데이터 표본 간격(~200ms) 밖으로 과도하게 외삽하면 노이즈가 증폭되므로,
    // 실제 표본 간격을 넘지 않게 캡을 씌운다.
    const double dt_pred = std::min(diff, dt_hist);

    std::vector<Detection> predicted = base->detections;
    for (auto& d : predicted) {
        const Detection* p = nullptr;
        for (const auto& pd : prev->detections) {
            if (pd.object_id == d.object_id) { p = &pd; break; }
        }
        if (!p) continue;  // 방금 나타난 객체 — 속도 미상, 원위치 유지

        const float dx = static_cast<float>((d.cx - p->cx) / dt_hist * dt_pred);
        const float dy = static_cast<float>((d.cy - p->cy) / dt_hist * dt_pred);

        d.left += dx;   d.right += dx;
        d.top += dy;    d.bottom += dy;
        d.cx += dx;     d.cy += dy;
    }
    return predicted;
}

std::vector<Detection> DetectionStore::latest(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(channel);
    if (it == history_.end() || it->second.empty()) return {};
    return it->second.back().detections;
}
