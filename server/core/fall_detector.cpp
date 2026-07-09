#include "fall_detector.hpp"

#include <cmath>
#include <iterator>
#include <map>

namespace {

// ── 잠정 기본값 (실측 캡처로 60fps/5fps 대역에 맞춤) ──────────
constexpr double kDropWindowSec = 1.2;      // 5fps 환경을 고려해 0.6초에서 1.2초로 확장 (약 6프레임 확보)
constexpr float kDropCyThreshold = 0.15f;   // 정규화 좌표 기준 무게중심 하강폭
constexpr double kStillSeconds = 3.0;       // 급하강 후 이만큼 안 움직이면 낙상 확정
constexpr float kStillMoveThreshold = 0.04f;  // 이 이하 이동은 "정지"로 간주
constexpr double kTrackExpireSec = 6.0;     // 이만큼 안 보인 추적은 폐기

// ── 머리(Head) 보조 신호 ─────────────────────────────────────
constexpr bool kHeadInstantTrigger = true;  // true=B(즉시), false=A(대기 단축)
constexpr float kHeadFloorCy = 0.80f;       // 머리 cy가 이보다 크면 "바닥 근처"
constexpr double kHeadStillSeconds = 1.0;   // (A모드) 머리 바닥 시 단축된 정지 대기

}  // namespace

void FallDetector::update(int channel, const std::vector<Detection>& detections) {
    auto& tracks = channels_[channel];
    auto now = std::chrono::steady_clock::now();

    // 이번 프레임의 머리 위치를 사람(Parent ObjectId)별로 모은다.
    std::map<int, float> head_cy;
    for (const auto& d : detections) {
        if (d.type == "Head" && d.parent_id != 0 && d.height() > 0) {
            head_cy[d.parent_id] = d.cy;
        }
    }

    for (const auto& d : detections) {
        // 사람만, 그리고 bbox 넓이 0인 요약 프레임은 제외
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;

        auto& tr = tracks[d.object_id];
        tr.last_seen = now;

        // [추가] 최초 진입 시 직전 좌표가 비어있다면 현재 좌표로 초기화
        if (tr.last_cx == 0.0f && tr.last_cy == 0.0f) {
            tr.last_cx = d.cx;
            tr.last_cy = d.cy;
        }

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
            } else {
                tr.last_cx = d.cx; // 다음 프레임을 위해 백업 후 통과
                tr.last_cy = d.cy;
                continue;
            }
        }

        // ── ⚠️ 수정 및 고도화된 정지/회복 판정 영역 ──────────────────
        
        // 1. 프레임 간 변위(속도) 계산 (최초 낙하지점이 아닌 '직전 프레임'과의 거리)
        float frame_moved = std::hypot(d.cx - tr.last_cx, d.cy - tr.last_cy);

        // 2. 일어났는지(회복 상태) 확인하는 마스터 룰
        // 종횡비가 서 있는 자세(<0.8)이고 무게중심이 바닥권(0.6) 위로 올라왔을 때
        bool stood_up = (d.aspectRatio() < 0.8f && d.cy < 0.6f);

        if (frame_moved > kStillMoveThreshold) {
            if (stood_up) {
                // 완전히 일어났다면 오탐으로 간주하고 관찰 종료(리셋)
                tr.watching = false;
                tr.last_cx = d.cx;
                tr.last_cy = d.cy;
                continue;
            }
            // 일어난 게 아니라 바닥에서 구르며 버둥거리는 상태라면, 
            // watching을 취소하지 않고 무시합니다. (tr.drop_time을 리셋하지 않음!)
        }

        // ─────────────────────────────────────────────────────────────

        // 이 사람의 머리가 바닥 근처인가? (보조 신호)
        auto hit = head_cy.find(d.object_id);
        bool head_near_floor = (hit != head_cy.end() && hit->second >= kHeadFloorCy);

        // 필요한 정지 지속시간: 머리 신호가 있으면 즉시(B) 또는 단축(A)
        double still_needed = kStillSeconds;
        if (head_near_floor) {
            still_needed = kHeadInstantTrigger ? 0.0 : kHeadStillSeconds;
        }

        double elapsed = std::chrono::duration<double>(now - tr.drop_time).count();
        if (elapsed >= still_needed && !tr.fired) {
            tr.fired = true;
            if (on_fall_) on_fall_(channel, d);
        }

        // [필수 추가] 다음 프레임 비교를 위해 현재 위치를 직전 위치로 저장
        tr.last_cx = d.cx;
        tr.last_cy = d.cy;
    }

    // 오래 안 보인 추적 정리
    for (auto it = tracks.begin(); it != tracks.end();) {
        double idle = std::chrono::duration<double>(now - it->second.last_seen).count();
        it = (idle > kTrackExpireSec) ? tracks.erase(it) : std::next(it);
    }
}