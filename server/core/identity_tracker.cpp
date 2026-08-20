#include "identity_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace {

// ── 인계(ID 스위치 보정) 튜닝값 ────────────────────────────────
// 트랙이 사라진 뒤 이 시간 안에 새 ID가 뜨면 같은 사람 후보로 본다.
// 길게 잡으면 옆 사람에게 라벨이 넘어가고, 짧게 잡으면 잠깐 가려진 것도 못 잇는다.
constexpr double kHandoverSec = 5.0;
// 인계로 인정할 최대 이동 거리(화면 폭 대비). 5초 동안 화면의 18%보다 멀리
// 간 것은 같은 사람이 아니라 다른 사람이 새로 들어온 것으로 본다.
constexpr float kHandoverDist = 0.18f;
// 그림자를 완전히 폐기하는 시간. 이보다 오래 안 보이면 인계 후보에서도 뺀다.
constexpr double kShadowExpireSec = 30.0;

// ── 신뢰도 튜닝값 ──────────────────────────────────────────────
// 처음 보인 자리가 이미 침대 안 = 그 침대에서 자고 있던 사람일 가능성이 가장 높다.
constexpr float kConfBorn = 0.90f;
// 미상으로 떠돌던 트랙이 침대로 걸어 들어옴 — 그 침대 사람일 확률이 높지만,
// 남의 침대에 앉은 방문객일 수도 있어 처음부터 있던 것보다는 낮게 준다.
constexpr float kConfEnter = 0.75f;
// 이미 A침대 사람으로 라벨된 트랙이 B침대로 들어감. 실제로 자리를 옮긴 것일 수도,
// ID가 옆 사람과 뒤바뀐 것일 수도 있어 신뢰도를 크게 떨어뜨린다(이름 표기 임계 아래).
constexpr float kConfBedSwap = 0.45f;
// 인계받은 라벨은 원본보다 이만큼 깎는다 (추론이 한 단계 더 들어갔으므로)
constexpr float kConfHandoverFactor = 0.8f;
// 침대 안에서 계속 보이면 초당 이만큼 상승 (상한 kConfMax)
constexpr float kConfGainPerSec = 0.02f;
constexpr float kConfMax = 0.95f;
// 침대 밖에 있으면 초당 이만큼 하강. 바닥은 kConfFloor.
// ★ 감쇠를 세게 걸면 안 된다. 낙상은 정의상 침대 밖에서 일어나고 화장실을 다녀오는
//   데만 수 분이 걸리는데, 초당 0.01(=40초면 0.9→0.5)로 깎으면 정작 이름을 붙여야
//   할 순간에 전부 "미상"이 되어 sticky 설계가 무의미해진다.
//   오귀속의 진짜 원인은 흐른 시간이 아니라 사건(ID 스왑·침대 이동·인계)이고,
//   그쪽은 kConfBedSwap / kConfHandoverFactor가 따로 떨군다. 시간은 약한 신호라
//   기울기만 남기고 바닥을 이름 표기 임계(kMinNameConfidence) 위에 둔다.
constexpr float kConfDecayPerSec = 0.002f;
// 이름 표기 임계(0.5)보다 살짝 위 — 한 번 제대로 귀속된 트랙은 침대 밖에 아무리
// 오래 있어도 이름을 유지한다. 반대로 인계를 여러 번 거쳤거나(0.8^3=0.46) 침대를
// 옮긴(0.45) 트랙은 이 바닥보다 낮은 채로 남아 "미상"으로 알린다.
constexpr float kConfFloor = 0.55f;

float dist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void IdentityTracker::update(int channel, const std::vector<Detection>& dets,
                             const std::map<int, BedZone>& zones) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& traces = channels_[channel];
    const auto now = std::chrono::steady_clock::now();

    std::vector<int> seen;
    seen.reserve(dets.size());

    for (const auto& d : dets) {
        // 사람만, 그리고 bbox 넓이 0인 요약 프레임(객체 소실 시 오는 것)은 제외
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;
        seen.push_back(d.object_id);

        const float fx = (d.left + d.right) / 2.0f;  // 발끝 — 침대 판정과 같은 기준점
        const float fy = d.bottom;
        const int zone_now = feetInWhichZone(d, zones);

        auto res = traces.emplace(d.object_id, Trace{});
        Trace& t = res.first->second;
        const bool is_new = res.second;

        if (is_new) {
            t.last_seen = now;
            // ── ③ 인계: 방금 사라진 트랙 중 이 자리에서 가장 가까운 것의 라벨을 물려받는다
            int best_id = -1;
            float best_dist = kHandoverDist;
            for (const auto& entry : traces) {
                if (entry.first == d.object_id) continue;
                const Trace& old = entry.second;
                if (!old.gone || old.resident_id <= 0) continue;
                const double idle = std::chrono::duration<double>(now - old.last_seen).count();
                if (idle > kHandoverSec) continue;
                const float dd = dist(fx, fy, old.x, old.y);
                if (dd < best_dist) { best_dist = dd; best_id = entry.first; }
            }
            if (best_id >= 0) {
                const Trace& old = traces[best_id];
                t.roi_id = old.roi_id;
                t.resident_id = old.resident_id;
                t.confidence = old.confidence * kConfHandoverFactor;
                t.caregiver = old.caregiver;
                // LOGOFF std::fprintf(stderr,
                //              "[id] ch%d obj%d ← obj%d 인계 (입소자 %d, 신뢰도 %.2f)\n",
                //              channel + 1, d.object_id, best_id, t.resident_id,
                //              t.confidence);
                traces.erase(best_id);  // 그림자는 역할을 다했다
            }
        }

        const double dt = std::chrono::duration<double>(now - t.last_seen).count();
        t.last_seen = now;
        t.gone = false;
        t.x = fx;
        t.y = fy;

        // ── ④ 제외: 보호사는 침대에 들어가도 귀속하지 않는다
        if (t.caregiver) {
            if (t.resident_id > 0) {
                std::fprintf(stderr, "[id] ch%d obj%d 보호사 판정 — 입소자 귀속 해제\n",
                             channel + 1, d.object_id);
            }
            t.roi_id = kNoZone;
            t.resident_id = 0;
            t.confidence = 0.f;
            continue;
        }

        if (zone_now == kNoZone) {
            // ── ② 유지: 침대 밖 — 라벨은 그대로 두고 신뢰도만 서서히 깎는다.
            // ★ 이미 바닥 이하인 트랙(침대 이동·다중 인계로 떨어진 것)은 건드리지
            //   않는다. max(floor, ...)를 그냥 걸면 0.45짜리가 바닥값 0.55로
            //   되올라가 미상이어야 할 트랙에 이름이 붙는다.
            if (t.resident_id > 0 && t.confidence > kConfFloor) {
                t.confidence = std::max(
                    kConfFloor, t.confidence - kConfDecayPerSec * static_cast<float>(dt));
            }
            continue;
        }

        // 침대 안 — 그 침대에 매핑된 입소자를 본다 (아직 사람이 안 붙었으면 0)
        int resident = 0;
        auto zit = zones.find(zone_now);
        if (zit != zones.end()) resident = zit->second.resident_id;

        if (t.roi_id == kNoZone) {
            // ── ①/귀속: 처음부터 침대 안이었나(born), 밖에서 걸어 들어왔나(enter)
            t.roi_id = zone_now;
            t.resident_id = resident;
            t.confidence = is_new ? kConfBorn : kConfEnter;
            // LOGOFF std::fprintf(stderr, "[id] ch%d obj%d → 침대%d 귀속 (입소자 %d, 신뢰도 %.2f)\n",
            //              channel + 1, d.object_id, zone_now + 1, resident, t.confidence);
        } else if (t.roi_id == zone_now) {
            // 계속 자기 침대에 있음 — 볼수록 확신이 는다
            t.resident_id = resident;  // 관리자가 매핑을 바꿨을 수 있으니 매번 갱신
            t.confidence = std::min(
                kConfMax, t.confidence + kConfGainPerSec * static_cast<float>(dt));
        } else {
            // 다른 침대로 이동 — 진짜 옮겼거나, ID가 옆 사람과 뒤바뀌었거나.
            // 어느 쪽인지 알 방법이 없으므로 라벨은 새 침대로 옮기되 신뢰도를 떨군다.
            // LOGOFF std::fprintf(stderr,
            //              "[id] ch%d obj%d 침대%d → 침대%d 이동 — 신뢰도 하향\n",
            //              channel + 1, d.object_id, t.roi_id + 1, zone_now + 1);
            t.roi_id = zone_now;
            t.resident_id = resident;
            t.confidence = kConfBedSwap;
        }
    }

    // ── 한 침대에 두 트랙이 귀속되는 것 방지 ──
    // 보호사 판정을 못 한 사람이 침대에 걸터앉으면 그 침대 입소자와 같은 라벨을
    // 달게 된다. 침대 하나엔 사람 하나 — 신뢰도가 높은 쪽만 라벨을 유지하고
    // 나머지는 미상으로 되돌린다(틀린 이름보다 미상이 낫다).
    std::map<int, int> owner;  // roi_id → 그 침대를 차지한 object_id
    for (auto& entry : traces) {
        Trace& t = entry.second;
        if (t.gone || t.roi_id == kNoZone || t.resident_id <= 0) continue;
        auto o = owner.find(t.roi_id);
        if (o == owner.end()) { owner[t.roi_id] = entry.first; continue; }

        Trace& rival = traces[o->second];
        Trace& loser = (t.confidence > rival.confidence) ? rival : t;
        if (t.confidence > rival.confidence) owner[t.roi_id] = entry.first;

        // LOGOFF std::fprintf(stderr, "[id] ch%d 침대%d 귀속 충돌 — obj 하나를 미상으로 되돌림\n",
        //              channel + 1, t.roi_id + 1);
        loser.roi_id = kNoZone;
        loser.resident_id = 0;
        loser.confidence = 0.f;
    }

    // ── 사라진 트랙 정리 ──
    // 바로 지우지 않고 그림자로 두었다가(인계 후보), 오래 지나면 폐기한다.
    for (auto it = traces.begin(); it != traces.end();) {
        const bool visible = std::find(seen.begin(), seen.end(), it->first) != seen.end();
        if (visible) { ++it; continue; }
        it->second.gone = true;
        const double idle = std::chrono::duration<double>(now - it->second.last_seen).count();
        it = (idle > kShadowExpireSec) ? traces.erase(it) : std::next(it);
    }
}

IdentityTracker::Identity IdentityTracker::identify(int channel, int object_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Identity out;
    auto ch = channels_.find(channel);
    if (ch == channels_.end()) return out;
    auto it = ch->second.find(object_id);
    if (it == ch->second.end()) return out;
    out.roi_id = it->second.roi_id;
    out.resident_id = it->second.resident_id;
    out.confidence = it->second.confidence;
    out.caregiver = it->second.caregiver;
    return out;
}

void IdentityTracker::markCaregivers(int channel, const std::vector<int>& object_ids) {
    if (object_ids.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& traces = channels_[channel];
    for (int id : object_ids) {
        // 메타데이터보다 프레임이 먼저 도착할 수 있어, 없으면 자리를 만들어 둔다.
        Trace& t = traces[id];
        if (t.caregiver) continue;   // 트랙당 한 번만 (sticky) — 로그도 한 번만 남는다
        t.caregiver = true;
        t.last_seen = std::chrono::steady_clock::now();
        // ★ 입소자가 안 붙은 트랙이어도 반드시 남긴다. 조용히 제외해 버리면
        //   "왜 이름이 안 뜨지"를 로그로 추적할 방법이 사라진다 — 보호사 판정은
        //   옷 색(HSV) 기반이라 오탐이 있고, 그게 이름 미표시의 흔한 원인이다.
        std::fprintf(stderr, "[id] ch%d obj%d 보호사 판정 — 침대 귀속 제외%s\n",
                     channel + 1, id,
                     t.resident_id > 0 ? " (기존 입소자 매핑 해제)" : "");
        t.roi_id = kNoZone;
        t.resident_id = 0;
        t.confidence = 0.f;
    }
}
