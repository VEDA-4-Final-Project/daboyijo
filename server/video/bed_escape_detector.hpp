#pragma once

#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>
#include <string>

#include "detection.hpp"

// 환자 위험 등급 (DB 연동용)
enum class PatientRisk {
    Low,    // 하: 탈출 알림 없음 (자유롭게 이동 가능, 낙상만 감시)
    Medium, // 중: 설정된 제한 시간대(예: 야간)에 이탈 시에만 즉시 알림
    High    // 상: 시간대에 상관없이 이탈 시 즉시 알림
};

class BedEscapeDetector {
public:
    // 알림 발생 시 외부로 이벤트를 던져줄 콜백 함수 포인터 타입
    using EscapeCallback = std::function<void(int channel, const Detection& detection)>;

    // 생성자: "중" 등급 환자의 기본 제한 시간대를 밤 10시(22) ~ 아침 6시(6)로 설정
    BedEscapeDetector(int escape_start_hour = 22, int escape_end_hour = 6, double track_expire_sec = 6.0)
        : escape_start_hour_(escape_start_hour),
          escape_end_hour_(escape_end_hour),
          track_expire_sec_(track_expire_sec) {}

    // 콜백 등록
    void registerOnEscape(EscapeCallback cb) { on_escape_ = cb; }

    // 매 프레임 업데이트
    void update(int channel,
                const std::vector<Detection>& detections,
                const std::vector<std::pair<float, float>>& bed_roi,
                PatientRisk channel_risk, //채널(침대)의 위험 등급을 주입
                bool caregiver_present); //요양보호사가 해당 병실에 재실 중인지 확인

private:
    struct EscapeTrack {
        std::chrono::steady_clock::time_point last_seen;
        bool in_bed = true;         // 현재 침대 내부인지 여부 (초기값은 true로 두어 최초 입장 시 오작동 방지)
        bool escape_fired = false;  // 알림 중복 발생 방지 플래그
    };

    // 현재 시간이 제한 시간대(야간)에 속하는지 판정하는 헬퍼 함수
    bool isWithinRestrictionTime() const;

    int escape_start_hour_;      // 탈출 감지 시작 시각
    int escape_end_hour_;        // 탈출 감지 종료 시각
    double track_expire_sec_;    // 트래킹 유효 시간
    EscapeCallback on_escape_ = nullptr;

    // 채널별 객체 추적 맵: channel -> { object_id -> EscapeTrack }
    std::unordered_map<int, std::unordered_map<int, EscapeTrack>> channels_;
};