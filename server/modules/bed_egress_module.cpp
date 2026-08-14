#include "bed_egress_module.hpp"
#include "database.hpp"

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <chrono>

namespace {
// 경계선 좌표 흔들림 노이즈로 1초에 알람이 수십 번 연타되는 것을 막는 쿨다운
constexpr double kAlarmCooldownSec = 10.0;
}  // namespace

void BedEgressModule::updatePatientStatus(int channel, int roi_id, int status) {
    if (status < 1 || status > 3) return;
    const auto value = static_cast<PatientStatus>(status);

    std::lock_guard<std::mutex> lock(mutex_);
    if (roi_id == BedZoneStore::kRoiIdAll) {
        // 채널 일괄 지정 — 아직 침대가 매핑되지 않은 입소자의 위험도가 이리로 온다.
        channel_default_[channel] = value;
        auto& ch = patient_statuses_[channel];
        for (auto& entry : ch) entry.second = value;
        std::printf("[BedEgress] 채널 %d 전체 위험도 갱신: %d\n", channel + 1, status);
        return;
    }
    patient_statuses_[channel][roi_id] = value;
    std::printf("[BedEgress] 채널 %d 침대 %d 위험도 갱신: %d\n",
                channel + 1, roi_id + 1, status);
}

PatientStatus BedEgressModule::statusOf(int channel, int roi_id) const {
    auto ch = patient_statuses_.find(channel);
    if (ch != patient_statuses_.end()) {
        auto it = ch->second.find(roi_id);
        if (it != ch->second.end()) return it->second;
    }
    // 침대별 값이 없으면 채널 일괄값 → 그것도 없으면 환자 안전을 위해
    // 기본값 최상위(HIGH). 모르면 울린다.
    auto def = channel_default_.find(channel);
    return def == channel_default_.end() ? PatientStatus::HIGH : def->second;
}

// 서버 구동 시 초기 데이터베이스 연동 및 환자 상태 동기화
void BedEgressModule::initializeFromDb(Database& db) {
    // 위험도의 진짜 소스는 residents.risk_level(Qt가 저장 시 기록하는 곳)이다.
    // 재시작해도 여기서 복원되므로, Qt에서 바꾼 '하'가 부팅 후에도 유지된다.
    // 침대마다 사람이 다르므로 조회도 침대 단위 — 그 침대에 매핑된 입소자의
    // 위험도를 읽고, 아직 사람이 안 붙은 침대는 채널 대표값으로 메운다.
    for (int ch = 0; ch < 4; ++ch) {
        const auto zones = zones_ ? zones_->zones(ch) : std::map<int, BedZone>{};
        if (zones.empty()) {
            std::fprintf(stderr, "[BedEgress] 부팅 위험도 로드: ch%d 침대 없음\n", ch + 1);
            continue;
        }

        const int channel_level = db.getRiskLevelByCamera(ch);  // 폴백용(-1이면 없음)

        for (const auto& entry : zones) {
            const BedZone& zone = entry.second;
            int level = -1;
            if (zone.resident_id > 0) level = db.getRiskLevelByResident(zone.resident_id);
            if (level < 1 || level > 3) level = channel_level;
            if (level < 1 || level > 3) level = static_cast<int>(PatientStatus::HIGH);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                patient_statuses_[ch][zone.roi_id] = static_cast<PatientStatus>(level);
            }

            // 버퍼링에 묻히지 않도록 stderr로 — 부팅 시 각 침대가 무슨 등급으로 떴는지 바로 보이게
            const char* label = (level == 3) ? "상" : (level == 2) ? "중" : "하";
            std::fprintf(stderr, "[BedEgress] 부팅 위험도 로드: ch%d 침대%d (입소자 %d) = %s\n",
                         ch + 1, zone.roi_id + 1, zone.resident_id, label);
        }
    }
}

void BedEgressModule::resetZoneState(int channel, int roi_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (roi_id == BedZoneStore::kRoiIdAll) {
        patient_statuses_.erase(channel);
        channel_default_.erase(channel);
        last_zone_.erase(channel);
        last_alarm_time_.erase(channel);
        return;
    }
    auto ch = patient_statuses_.find(channel);
    if (ch != patient_statuses_.end()) ch->second.erase(roi_id);
    auto lz = last_zone_.find(channel);
    if (lz != last_zone_.end()) {
        for (auto& entry : lz->second) {
            if (entry.second == roi_id) entry.second = kNoZone;
        }
    }
}

// 22:00부터 익일 05:59:59까지 야간으로 판정
bool BedEgressModule::isNightTime() const {
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    return (now->tm_hour >= 22 || now->tm_hour < 6);
}

void BedEgressModule::processDetections(int channel, const std::vector<Detection>& dets) {
    // 침대 스냅샷은 락 밖에서 (zones_는 자체 락)
    if (!zones_) return;
    const auto zones = zones_->zones(channel);

    bool any_valid = false;
    for (const auto& entry : zones) {
        if (entry.second.valid()) { any_valid = true; break; }
    }
    if (!any_valid) {
        // 이 채널엔 아직 그려진 침대가 없다 — 판정할 기준이 없으니 상태만 비우고 끝.
        std::lock_guard<std::mutex> lock(mutex_);
        last_zone_.erase(channel);
        last_alarm_time_.erase(channel);
        return;
    }

    const bool night = isNightTime();
    const auto now = std::chrono::steady_clock::now();

    std::vector<int> current_obj_ids;
    std::vector<EgressEvent> triggers;  // 락 밖에서 안전하게 콜백을 쏘기 위한 임시 보관함

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& last_zone = last_zone_[channel];

        for (const auto& det : dets) {
            // 사람 객체만 검사
            if (!det.isHuman()) continue;
            current_obj_ids.push_back(det.object_id);

            const int zone_now = feetInWhichZone(det, zones);

            // 이 객체 ID를 시스템이 처음 본 경우 상태 등록 후 통과
            auto it = last_zone.find(det.object_id);
            if (it == last_zone.end()) {
                last_zone[det.object_id] = zone_now;
                continue;
            }

            const int zone_before = it->second;
            it->second = zone_now;  // 다음 프레임 비교를 위해 현재 상태 저장

            // 침대 안에 있다가 밖으로 나갔을 때만 이탈. 침대 A → 침대 B로
            // 곧장 넘어간 경우(zone_now != kNoZone)는 침상 사이 이동으로 보고
            // 경보를 울리지 않는다 — 그래도 사람이 바뀐 셈이라 기록은 남긴다.
            if (zone_before == kNoZone || zone_before == zone_now) continue;
            if (zone_now != kNoZone) {
                std::fprintf(stderr, "[BedEgress] ch%d obj%d 침대%d → 침대%d 이동\n",
                             channel + 1, det.object_id, zone_before + 1, zone_now + 1);
                continue;
            }

            // ── [환자 위험도별 알림 필터링] ──
            // 위험도는 침대(=사람)마다 다르다. 같은 방에 '하'와 '상'이 같이 누워
            // 있어도 '상'인 사람의 이탈은 반드시 울려야 한다.
            const PatientStatus status = statusOf(channel, zone_before);
            if (status == PatientStatus::LOW) continue;            // 하: 이탈해도 알림 없음
            if (status == PatientStatus::MEDIUM && !night) continue;  // 중: 야간에만

            // [디바운스/쿨다운] 경계선 좌표 흔들림으로 알람이 연타되는 것 방지
            auto& last_time = last_alarm_time_[channel][det.object_id];
            if (std::chrono::duration<double>(now - last_time).count() < kAlarmCooldownSec)
                continue;
            last_time = now;

            EgressEvent evt;
            evt.channel = channel;
            evt.object_id = det.object_id;
            evt.roi_id = zone_before;
            auto zit = zones.find(zone_before);
            evt.resident_id = (zit != zones.end()) ? zit->second.resident_id : 0;
            evt.x = (det.left + det.right) / 2.0f;
            evt.y = det.bottom;
            triggers.push_back(evt);   // 보관만 해둠
        }

        // ── [메모리 관리 - 누수 방지] ──
        // 알림 차단 여부와 무관하게 무조건 실행되어 화면에서 사라진 ID 제거
        for (auto it = last_zone.begin(); it != last_zone.end();) {
            if (std::find(current_obj_ids.begin(), current_obj_ids.end(), it->first) ==
                current_obj_ids.end()) {
                last_alarm_time_[channel].erase(it->first);  // 쿨다운 맵도 함께 청소
                it = last_zone.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── [데드락 방지 - 안전 지대] ──
    // 락이 완전히 풀린 상태이므로 여기서 소켓 전송/블랙박스 연동을 해도 절대 서버가 멈추지 않습니다.
    for (const auto& evt : triggers) {
        const PatientStatus status = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            return statusOf(evt.channel, evt.roi_id);
        }();
        const char* status_str = (status == PatientStatus::HIGH) ? "🔴 상(즉시 경보)"
                                                                 : "🟠 중(야간 관찰)";
        std::fprintf(stderr,
                     "⚠️ [ch%d] [%s] 침대%d 환자 침상 탈출 발생! (obj: %d, 입소자: %d)\n",
                     evt.channel + 1, status_str, evt.roi_id + 1, evt.object_id,
                     evt.resident_id);

        if (alarm_cb_) alarm_cb_(evt);
    }
}
