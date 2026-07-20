#include "bed_egress_module.hpp"

#include <algorithm>
#include <ctime>

void BedEgressModule::setRiskLevel(int channel, PatientRisk risk) {
    std::lock_guard<std::mutex> lock(mutex_);
    risk_levels_[channel] = risk;
}

// Qt 클라이언트가 전송한 ROI를 로컬 메모리에 동기화
void BedEgressModule::updateBedRoi(int channel, bool clear, const std::vector<std::pair<float, float>>& points) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clear) {
        rois_.erase(channel);
    } else {
        rois_[channel] = points;
    }
}

// 22:00부터 익일 05:59:59까지 야간으로 판정
bool BedEgressModule::isNightTime() const {
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    return (now->tm_hour >= 22 || now->tm_hour < 6);
}

void BedEgressModule::processDetections(int channel, const std::vector<Detection>& dets) {
    std::vector<std::pair<float, float>> roi;
    PatientRisk risk = PatientRisk::HIGH; // 안전을 위해 기본값 '상'

    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 설정된 ROI가 없거나 점이 3개 미만(도형 성립 불가)이면 검사 생략
        auto it = rois_.find(channel);
        if (it == rois_.end() || it->second.size() < 3) return; 
        roi = it->second;

        // 지정된 위험도가 있다면 가져옴
        if (risk_levels_.count(channel)) {
            risk = risk_levels_[channel];
        }
    }

    // ── [알림 필터링] ──
    // 1. 위험도 '하': 이탈 알림 무시
    if (risk == PatientRisk::LOW) return;
    // 2. 위험도 '중': 야간 시간이 아니면 알림 무시
    if (risk == PatientRisk::MEDIUM && !isNightTime()) return;


    // ── [침상 탈출 감지 로직] ──
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> current_obj_ids;

    for (const auto& det : dets) {
        // 사람 객체만 검사
        if (!det.isHuman()) continue;
        current_obj_ids.push_back(det.object_id);

        // ★ detection.hpp에 정의된 공용 헬퍼 함수 활용!
        bool is_currently_in_bed = isFeetInRoi(det, roi);
        
        // 이전 상태 가져오기 (처음 감지된 사람은 일단 침상 안이라고 가정 -> 오알람 방지)
        bool was_in_bed = true; 
        if (is_in_bed_[channel].count(det.object_id)) {
            was_in_bed = is_in_bed_[channel][det.object_id];
        }

        // [핵심] 이전 프레임에는 침상 안에 있었는데, 이번 프레임에서 밖으로 나갔다면!
        if (was_in_bed && !is_currently_in_bed) {
            if (alarm_cb_) {
                alarm_cb_(channel, det.object_id); // 알림 발송
            }
        }
        
        // 현재 상태를 다음 프레임 비교를 위해 저장
        is_in_bed_[channel][det.object_id] = is_currently_in_bed;
    }

    // ── [메모리 관리] ──
    // 카메라 화면에서 완전히 사라진 사람(ID)은 상태 맵에서 지워 메모리 누수를 방지
    for (auto it = is_in_bed_[channel].begin(); it != is_in_bed_[channel].end(); ) {
        if (std::find(current_obj_ids.begin(), current_obj_ids.end(), it->first) == current_obj_ids.end()) {
            it = is_in_bed_[channel].erase(it);
        } else {
            ++it;
        }
    }
}