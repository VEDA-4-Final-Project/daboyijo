#include "bed_egress_module.hpp"
#include "database.hpp"
#include "identity_tracker.hpp"   // 재실에서 요양보호사를 빼기 위해

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <chrono>

namespace {
// 경계선 좌표 흔들림 노이즈로 1초에 알람이 수십 번 연타되는 것을 막는 쿨다운
constexpr double kAlarmCooldownSec = 10.0;

// [재실] 침대가 이만큼 연속으로 비어 보여야 "나갔다"로 친다.
// 카메라는 이불을 덮고 자는 사람을 프레임 단위로 자주 놓친다 — 한 프레임 안 보인다고
// 바로 세션을 닫으면 하룻밤에 세션이 수백 개 생기고 재실시간도 그만큼 잘게 쪼개진다.
// 반대로 너무 길면 화장실 다녀온 짧은 이탈이 재실로 묻힌다. 10초는 그 사이 값이다.
constexpr double kVacancyConfirmSec = 10.0;
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
        // LOGOFF std::printf("[BedEgress] 채널 %d 전체 위험도 갱신: %d\n", channel + 1, status);
        return;
    }
    patient_statuses_[channel][roi_id] = value;
    // LOGOFF std::printf("[BedEgress] 채널 %d 침대 %d 위험도 갱신: %d\n",
    //             channel + 1, roi_id + 1, status);
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
            // LOGOFF std::fprintf(stderr, "[BedEgress] 부팅 위험도 로드: ch%d 침대 없음\n", ch + 1);
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
            // LOGOFF const char* label = (level == 3) ? "상" : (level == 2) ? "중" : "하";
            // LOGOFF std::fprintf(stderr, "[BedEgress] 부팅 위험도 로드: ch%d 침대%d (입소자 %d) = %s\n",
            //              ch + 1, zone.roi_id + 1, zone.resident_id, label);
        }
    }
}

void BedEgressModule::resetZoneState(int channel, int roi_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // ★ 침대를 지우면 그 침대의 열린 재실 세션도 닫는다. 안 닫으면 그 세션은
    //   영영 열린 채로 남아 리포트에서 "지금까지 계속 누워있음"으로 읽힌다.
    auto closeOpen = [&](int roi) {
        auto ch = open_sessions_.find(channel);
        if (ch == open_sessions_.end()) return;
        if (roi == BedZoneStore::kRoiIdAll) {
            if (db_) for (auto& z : ch->second) if (z.second > 0) db_->closeBedSession(z.second);
            ch->second.clear();
        } else {
            auto z = ch->second.find(roi);
            if (z != ch->second.end()) {
                if (db_ && z->second > 0) db_->closeBedSession(z->second);
                ch->second.erase(z);
            }
        }
    };

    if (roi_id == BedZoneStore::kRoiIdAll) {
        closeOpen(BedZoneStore::kRoiIdAll);
        empty_since_.erase(channel);
        patient_statuses_.erase(channel);
        channel_default_.erase(channel);
        last_zone_.erase(channel);
        last_alarm_time_.erase(channel);
        return;
    }
    closeOpen(roi_id);
    auto es = empty_since_.find(channel);
    if (es != empty_since_.end()) es->second.erase(roi_id);
    auto ch = patient_statuses_.find(channel);
    if (ch != patient_statuses_.end()) ch->second.erase(roi_id);
    auto lz = last_zone_.find(channel);
    if (lz != last_zone_.end()) {
        for (auto& entry : lz->second) {
            if (entry.second == roi_id) entry.second = kNoZone;
        }
    }
}

// ── [일일 리포트] 침대 재실 세션 열기/닫기 ────────────────────────
// mutex_ 를 쥔 채 호출된다.
//
// ★ DB 호출을 락 안에서 하는 이유: 세션 전환은 드물다(침대당 하루 몇 번). 알람
//   콜백처럼 소켓 전송·블랙박스·텔레그램을 줄줄이 하는 무거운 경로가 아니라
//   INSERT/UPDATE 한 방이라, 락을 잠깐 쥐는 편이 "락 풀고 → DB 호출 → 다시 락 잡아
//   session_id 저장" 하는 것보다 단순하고 그 사이에 끼어드는 경우도 없다.
//   Database 는 자체 뮤텍스를 쓰고 이 모듈을 되부르지 않으므로 교착도 없다.
void BedEgressModule::syncBedSessions(int channel, const std::map<int, BedZone>& zones,
                                      const std::map<int, int>& occupancy,
                                      std::chrono::steady_clock::time_point now) {
    auto& open = open_sessions_[channel];
    auto& empty = empty_since_[channel];

    for (const auto& entry : zones) {
        const BedZone& zone = entry.second;
        if (!zone.valid()) continue;                 // 그리다 만 침대는 판정 대상 아님

        const int roi = zone.roi_id;
        const auto occ = occupancy.find(roi);
        const bool now_occupied = (occ != occupancy.end() && occ->second > 0);
        const long long open_id = open.count(roi) ? open[roi] : 0;

        if (now_occupied) {
            empty.erase(roi);                        // 다시 찼으니 빈 시각 취소
            if (open_id == 0) {
                // 침대에 사람이 들어왔다 — 재실 시작.
                // 사람은 침대에 매핑된 입소자다(0 이면 미지정 → resident_id NULL).
                const long long id = db_->openBedSession(channel, zone.resident_id);
                if (id > 0) {
                    open[roi] = id;
                    std::fprintf(stderr, "[BedEgress] ch%d 침대%d 재실 시작 (입소자 %d)\n",
                                 channel + 1, roi + 1, zone.resident_id);
                }
            }
            continue;
        }

        // 비어 보인다 — 다만 곧바로 닫지 않는다(이불·가림으로 인한 순간 미검출).
        if (open_id == 0) continue;                  // 원래 비어 있던 침대
        auto since = empty.find(roi);
        if (since == empty.end()) { empty[roi] = now; continue; }   // 방금부터 비어 보임
        if (std::chrono::duration<double>(now - since->second).count() < kVacancyConfirmSec)
            continue;                                // 아직 확정 전 — 더 지켜본다

        if (db_->closeBedSession(open_id)) {
            std::fprintf(stderr, "[BedEgress] ch%d 침대%d 재실 종료\n", channel + 1, roi + 1);
        }
        open.erase(roi);
        empty.erase(roi);
    }
}

void BedEgressModule::flushBedSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    int n = 0;
    for (auto& ch : open_sessions_) {
        for (auto& z : ch.second)
            if (z.second > 0 && db_->closeBedSession(z.second)) ++n;
        ch.second.clear();
    }
    empty_since_.clear();
    if (n > 0) std::fprintf(stderr, "[BedEgress] 종료 — 열린 재실 세션 %d건 마감\n", n);
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

    // ── [일일 리포트] 이번 프레임의 침대별 재실 인원 ──
    // 전이(안→밖)가 아니라 "지금 몇 명 있나"로 세는 이유: 추적 ID 는 사람이 가려지면
    // 바뀌는데, 인원수는 ID 가 바뀌어도 그대로다. 전이 기반으로 세면 자다가 ID 가
    // 갈릴 때마다 세션이 닫혔다 열린다.
    // 요양보호사는 뺀다 — 침대에 걸터앉으면 발끝이 ROI 안으로 들어와, 그대로 두면
    // "환자가 누워있던 시간"에 방문 시간이 섞인다.
    std::map<int, int> occupancy;   // roi_id → 인원
    if (db_) {
        for (const auto& det : dets) {
            if (!det.isHuman()) continue;
            if (identity_ && identity_->identify(channel, det.object_id).caregiver) continue;
            const int z = feetInWhichZone(det, zones);
            if (z != kNoZone) occupancy[z]++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 알람 판정보다 먼저 — 재실 기록은 위험도·야간과 무관하게 항상 남아야 한다.
        if (db_) syncBedSessions(channel, zones, occupancy, now);

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
                // LOGOFF std::fprintf(stderr, "[BedEgress] ch%d obj%d 침대%d → 침대%d 이동\n",
                //              channel + 1, det.object_id, zone_before + 1, zone_now + 1);
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
        (void)status;  // LOGOFF 로그만 쓰던 값
        // LOGOFF const char* status_str = (status == PatientStatus::HIGH) ? "🔴 상(즉시 경보)"
        //                                                          : "🟠 중(야간 관찰)";
        // LOGOFF std::fprintf(stderr,
        //              "⚠️ [ch%d] [%s] 침대%d 환자 침상 탈출 발생! (obj: %d, 입소자: %d)\n",
        //              evt.channel + 1, status_str, evt.roi_id + 1, evt.object_id,
        //              evt.resident_id);

        if (alarm_cb_) alarm_cb_(evt);
    }
}
