#include "fall_detector.hpp"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>

namespace {

// 점 (px,py)가 정규화 다각형 poly 안에 있는지 (ray-casting). 침상 재실/이탈 판정.
bool pointInPolygon(float px, float py,
                    const std::vector<std::pair<float, float>>& poly) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const float xi = poly[i].first, yi = poly[i].second;
        const float xj = poly[j].first, yj = poly[j].second;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

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

void FallDetector::update(int channel, const std::vector<Detection>& detections,
                          const std::vector<std::pair<float, float>>& bed_roi) {
    auto& tracks = channels_[channel];
    auto now = std::chrono::steady_clock::now();
    const bool has_bed = bed_roi.size() >= 3;  // 3점 미만이면 게이트 없음(폴백)

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

        // ── ⓪ 침대 ROI 게이팅 ───────────────────────────────────
        // 침대가 설정된 채널은 "침대 밖(관찰모드)"일 때만 낙상을 본다.
        // 침대 안에서는 취침·뒤척임을 전부 무시하고 상태를 리셋한다.
        // 침대가 없으면(has_bed=false) 폴백으로 화면 전체를 관찰모드로 본다.
        const bool in_bed = has_bed && pointInPolygon(d.cx, d.cy, bed_roi);
        if (in_bed) {
            if (!tr.in_bed) {
                std::fprintf(stderr, "[fall] ch%d obj%d 침상 재실 — 관찰 중단\n",
                             channel, d.object_id);
            }
            tr.in_bed = true;
            tr.watching = false;
            tr.fired = false;
            tr.history.clear();
            tr.last_cx = 0.0f;  // 다음 이탈 시 프레임 변위가 새로 시작되게
            tr.last_cy = 0.0f;
            continue;
        }
        if (tr.in_bed) {
            // 방금 침대에서 나옴 → 관찰모드 진입. 침대에서 내려오는 동작 자체가
            // 급하강으로 오탐되지 않도록 이력·기준을 리셋하고 이후의 하강만 본다.
            std::fprintf(stderr, "[fall] ch%d obj%d 침상 이탈 → 관찰모드\n",
                         channel, d.object_id);
            tr.in_bed = false;
            tr.watching = false;
            tr.fired = false;
            tr.history.clear();
            tr.last_cx = 0.0f;
            tr.last_cy = 0.0f;
        }

        // [추가] 최초 진입 시(또는 이탈 리셋 직후) 직전 좌표를 현재로 초기화
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