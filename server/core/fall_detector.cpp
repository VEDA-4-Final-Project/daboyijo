#include "fall_detector.hpp"

#include <cmath>
#include <iterator>

namespace {

// ── 잠정 기본값 (실측 캡처로 튜닝 전) ────────────────────────
// 걷기/천천히눕기/급쓰러짐 캡처 확보 후 이 파일만 고치면 된다.
constexpr double kDropWindowSec = 0.6;      // 이 시간 안의 하강만 "급함"으로 침
constexpr float kDropCyThreshold = 0.15f;   // 정규화 좌표 기준 무게중심 하강폭
constexpr double kStillSeconds = 3.0;       // 급하강 후 이만큼 안 움직이면 낙상 확정
constexpr float kStillMoveThreshold = 0.04f;  // 이 이하 이동은 "정지"로 간주
constexpr double kTrackExpireSec = 6.0;     // 이만큼 안 보인 추적은 폐기

}  // namespace

void FallDetector::update(int channel, const std::vector<Detection>& detections) {
    auto& tracks = channels_[channel];
    auto now = std::chrono::steady_clock::now();

    for (const auto& d : detections) {
        // 사람만, 그리고 bbox 넓이 0인 요약 프레임(객체 소실 시 오는 것)은 제외
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;

        auto& tr = tracks[d.object_id];
        tr.last_seen = now;
        tr.history.push_back({now, d.cx, d.cy});
        while (!tr.history.empty() &&
               std::chrono::duration<double>(now - tr.history.front().t).count() >
                   kDropWindowSec) {
            tr.history.pop_front();
        }

        if (!tr.watching) {
            // 최근 창에서 가장 높이 있던 위치(cy 최소) 대비 현재 하강폭 확인
            float min_cy = tr.history.front().cy;
            for (const auto& s : tr.history) min_cy = std::min(min_cy, s.cy);
            if (d.cy - min_cy >= kDropCyThreshold) {
                tr.watching = true;
                tr.fired = false;
                tr.drop_time = now;
                tr.drop_cx = d.cx;
                tr.drop_cy = d.cy;
            }
            continue;
        }

        // 관찰 중: 급하강 시점 위치에서 계속 안 움직이는지 확인
        float moved = std::hypot(d.cx - tr.drop_cx, d.cy - tr.drop_cy);
        if (moved > kStillMoveThreshold) {
            tr.watching = false;  // 다시 움직임 = 낙상 아님(앉았다 자세 고침 등)
            continue;
        }

        double elapsed = std::chrono::duration<double>(now - tr.drop_time).count();
        if (elapsed >= kStillSeconds && !tr.fired) {
            tr.fired = true;
            if (on_fall_) on_fall_(channel, d);
        }
    }

    // 오래 안 보인 추적 정리 (사람이 화면을 떠났거나 새 ID로 재등장한 경우)
    for (auto it = tracks.begin(); it != tracks.end();) {
        double idle = std::chrono::duration<double>(now - it->second.last_seen).count();
        it = (idle > kTrackExpireSec) ? tracks.erase(it) : std::next(it);
    }
}
