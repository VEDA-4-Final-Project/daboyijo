#include "fall_detector.hpp"

#include <cstdio>
#include <iterator>

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

constexpr double kLyingConfirmSec = 2.0;  // 누운 자세가 이만큼 연속돼야 낙상 확정 (잠정값)
constexpr double kTrackExpireSec = 6.0;   // 이만큼 안 보인 추적은 폐기
// 알림 후 "서있음"이 이만큼 연속 유지되면 재무장(fired 리셋) — 부축받아 일어난
// 사람이 침대에 안 들르고 다시 낙상해도 재알림되도록. 짧으면 누운 사람의 자세
// 오분류 한 번에 재무장→중복 알림이 나므로 여유 있게 잡는다 (잠정값).
constexpr double kStandingRearmSec = 5.0;

}  // namespace

void FallDetector::update(int channel, const std::vector<Detection>& detections,
                          const std::vector<std::pair<float, float>>& bed_roi) {
    auto& tracks = channels_[channel];
    auto now = std::chrono::steady_clock::now();
    const bool has_bed = bed_roi.size() >= 3;  // 3점 미만이면 게이트 없음(폴백)

    for (const auto& d : detections) {
        // 사람만, 그리고 bbox 넓이 0인 요약 프레임(객체 소실 시 오는 것)은 제외
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;

        auto& tr = tracks[d.object_id];
        tr.last_seen = now;
        tr.left = d.left;
        tr.top = d.top;
        tr.right = d.right;
        tr.bottom = d.bottom;

        // 침대 ROI 게이팅: 침대 안이면 관찰 대상에서 빠지고 상태 리셋.
        const bool in_bed = has_bed && pointInPolygon(d.cx, d.cy, bed_roi);
        if (in_bed) {
            if (!tr.in_bed) {
                std::fprintf(stderr, "[fall] ch%d obj%d 침상 재실 — 관찰 중단\n",
                             channel, d.object_id);
            }
            tr.in_bed = true;
            tr.lying_active = false;
            tr.standing_active = false;
            tr.fired = false;
            continue;
        }
        if (tr.in_bed) {
            std::fprintf(stderr, "[fall] ch%d obj%d 침상 이탈 → 관찰모드\n",
                         channel, d.object_id);
        }
        tr.in_bed = false;
    }

    // 오래 안 보인 추적 정리
    for (auto it = tracks.begin(); it != tracks.end();) {
        double idle = std::chrono::duration<double>(now - it->second.last_seen).count();
        it = (idle > kTrackExpireSec) ? tracks.erase(it) : std::next(it);
    }
}

std::vector<FallDetector::ObservedTrack> FallDetector::observedTracks(int channel) const {
    std::vector<ObservedTrack> result;
    auto ch_it = channels_.find(channel);
    if (ch_it == channels_.end()) return result;
    for (const auto& [id, tr] : ch_it->second) {
        if (tr.in_bed) continue;
        result.push_back({id, tr.left, tr.top, tr.right, tr.bottom});
    }
    return result;
}

void FallDetector::reportPose(int channel, int object_id, bool lying) {
    auto ch_it = channels_.find(channel);
    if (ch_it == channels_.end()) return;
    auto tr_it = ch_it->second.find(object_id);
    if (tr_it == ch_it->second.end()) return;
    Track& tr = tr_it->second;
    if (tr.in_bed) return;  // 자세 확인 중 그 사이 침대로 복귀했으면 무시

    auto now = std::chrono::steady_clock::now();
    if (!lying) {
        tr.lying_active = false;
        // 알림 후 기립이 연속 유지되면 재무장 — 침대 재실 없이 두 번째 낙상이
        // 발생해도 다시 알릴 수 있어야 한다 (누움 관측이 끼면 연속성 리셋).
        if (tr.fired) {
            if (!tr.standing_active) {
                tr.standing_active = true;
                tr.standing_since = now;
            } else if (std::chrono::duration<double>(now - tr.standing_since).count() >=
                       kStandingRearmSec) {
                tr.fired = false;
                tr.standing_active = false;
                std::fprintf(stderr,
                             "[fall] ch%d obj%d 기립 %.0f초 유지 — 낙상 감시 재무장\n",
                             channel, object_id, kStandingRearmSec);
            }
        }
        return;
    }
    tr.standing_active = false;  // 누움 관측 — 기립 연속성 끊김
    if (!tr.lying_active) {
        tr.lying_active = true;
        tr.lying_since = now;
        return;  // 이번이 첫 관측 — 아직 확정 안 함(연속성 확인 필요)
    }

    double elapsed = std::chrono::duration<double>(now - tr.lying_since).count();
    if (elapsed >= kLyingConfirmSec && !tr.fired) {
        tr.fired = true;
        if (on_fall_) {
            Detection at;
            at.object_id = object_id;
            at.cx = (tr.left + tr.right) / 2.0f;
            at.cy = (tr.top + tr.bottom) / 2.0f;
            at.type = "Human";
            on_fall_(channel, at);
        }
    }
}
