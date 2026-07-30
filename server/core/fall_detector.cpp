#include "fall_detector.hpp"
#include "detection.hpp"

#include <cstdio>
#include <iterator>

namespace {

constexpr double kLyingConfirmSec = 2.0;  // 누운 자세가 이만큼 연속돼야 낙상 확정 (잠정값)
constexpr double kTrackExpireSec = 6.0;   // 이만큼 안 보인 추적은 폐기

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
        // 판정 기준점은 무게중심(cx,cy)이 아니라 bbox 하단 중앙("발끝").
        // 무게중심은 몸통 높이에 떠 있어 침대 "앞"에 선 사람도 2D 화면상
        // ROI와 겹쳐 재실로 오판된다. 발끝은 바닥 평면에 붙은 점이라 화면
        // y좌표가 실제 3D 위치를 반영 — 침대 앞이면 ROI 아래(밖), 침대에
        // 누우면 ROI 안으로 갈린다. (ROI는 침대 발자국 기준으로 그릴 것)
        const bool in_bed = has_bed && isFeetInRoi(d, bed_roi);
        
        if (in_bed) {
            if (!tr.in_bed) {
                std::fprintf(stderr, "[fall] ch%d obj%d 침상 재실 — 관찰 중단\n",
                             channel + 1, d.object_id);
            }
            tr.in_bed = true;
            tr.lying_active = false;
            tr.fired = false;
            continue;
        }
        if (tr.in_bed) {
            std::fprintf(stderr, "[fall] ch%d obj%d 침상 이탈 → 관찰 시작\n",
                         channel + 1, d.object_id);
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
        return;
    }
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
