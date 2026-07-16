#include "bed_escape_detector.hpp"
#include "core/roi_utils.hpp"
#include <cstdio>
#include <ctime>
#include <iterator>

// 현재 시간이 설정된 제한 시간대 범위에 속하는지 판정
bool BedEscapeDetector::isWithinRestrictionTime() const {
    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);
    int current_hour = local->tm_hour;

    // 야간 시간대처럼 자정을 넘어가는 경우 (예: 22시 ~ 다음날 06시)
    if (escape_start_hour_ > escape_end_hour_) {
        return (current_hour >= escape_start_hour_ || current_hour < escape_end_hour_);
    } 
    // 동일한 날짜 안의 범위인 경우 (예: 13시 ~ 18시)
    else {
        return (current_hour >= escape_start_hour_ && current_hour < escape_end_hour_);
    }
}

void BedEscapeDetector::update(int channel,
                               const std::vector<Detection>& detections,
                               const std::vector<std::pair<float, float>>& bed_roi,
                               PatientRisk channel_risk,
                               bool caregiver_present) {
    auto& tracks = channels_[channel];
    auto now = std::chrono::steady_clock::now();

    // 이번 프레임의 야간 제한 시간대 여부 판정
    const bool is_restriction_active = isWithinRestrictionTime();

    for (const auto& d : detections) {
        // 객체가 사람이 아니거나 크기가 비정상적이면 무시
        if (!d.isHuman() || d.width() <= 0 || d.height() <= 0) continue;

        auto& tr = tracks[d.object_id];
        tr.last_seen = now;

        const bool in_bed = RoiUtils::isFootInBed(d, bed_roi);
        
        // 요양보호사 돌봄 상태 예외 처리
        if (caregiver_present) {
            tr.in_bed = in_bed;
            tr.escape_fired = false; // 보호사가 지켜보는 중이므로 알림 발생 여부 리셋
            continue;
        }

        // 환자가 침대 안(in_bed)에 있을 때의 처리
        if (in_bed) {
            if (!tr.in_bed) {
                std::fprintf(stderr, "[escape] ch%d obj%d 침상 재실 — 탈출 관찰 중단\n", channel, d.object_id);
            }
            tr.in_bed = true;
            tr.escape_fired = false; // 다시 누웠으므로 알림 플래그 리셋
            continue;
        }

        // 환자가 침대 밖(이탈 상태)에 있을 때의 처리
        if (tr.in_bed) {
            tr.in_bed = false;
            bool trigger_alert = false;

            // 위험 등급에 따른 알림 트리거 조건 분기
            if (channel_risk == PatientRisk::High) {
                // "상": 무조건 즉시 알림
                trigger_alert = true;
                std::fprintf(stderr, "[ALERT] ch%d obj%d '상' 등급 환자 침상 즉시 이탈 감지!\n", channel, d.object_id);
            } 
            else if (channel_risk == PatientRisk::Medium) {
                // "중": 설정된 제한 시간대(야간 등)에만 즉시 알림
                if (is_restriction_active) {
                    trigger_alert = true;
                    std::fprintf(stderr, "[ALERT] ch%d obj%d '중' 등급 환자 야간 제한 시간대(%d시~%d시) 이탈 감지!\n",
                                 channel, d.object_id, escape_start_hour_, escape_end_hour_);
                } else {
                    std::fprintf(stderr, "[escape] ch%d obj%d '중' 등급 환자 주간 이탈\n", channel, d.object_id);
                }
            }
            // "하" 등급은 아무 동작 안 함 (알림 조건 성립 안 됨)

            // 알림 콜백 실행 (한 번의 이탈에 한 번만 알림이 가도록 제어)
            if (trigger_alert && !tr.escape_fired) {
                tr.escape_fired = true;
                if (on_escape_) {
                    on_escape_(channel, d);
                }
            }
        }
    }

    // 시야에서 사라진 지 오래된 객체(추적 유실) 메모리 정리
    for (auto it = tracks.begin(); it != tracks.end();) {
        double idle = std::chrono::duration<double>(now - it->second.last_seen).count();
        it = (idle > track_expire_sec_) ? tracks.erase(it) : std::next(it);
    }
}