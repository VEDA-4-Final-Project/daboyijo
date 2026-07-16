#include "bed_escape_module.hpp"
#include <mutex>

BedEscapeModule::BedEscapeModule() 
    // 기본 생성자로 야간 시간대(22시~6시) 및 트랙 만료 시간(6초) 세팅
    : detector_(22, 6, 6.0) 
{
    // 저수준 Detector에서 이탈 이벤트가 터지면, 이 모듈이 받아서 고수준 콜백으로 포워딩
    detector_.registerOnEscape([this](int channel, const Detection& d) {
        EscapeCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = on_escape_;
        }
        if (cb) {
            cb(channel, d);
        }
    });
}

void BedEscapeModule::setEscapeCallback(EscapeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_escape_ = std::move(cb);
}

void BedEscapeModule::updateBedRoi(int channel, bool clear, const std::vector<std::pair<float, float>>& points) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clear) {
        bed_rois_[channel].clear();
    } else {
        bed_rois_[channel] = points;
    }
}

void BedEscapeModule::onMetadata(int channel, const std::vector<Detection>& detections, bool caregiver_present) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 보관 중인 해당 채널의 ROI 데이터 확보
    const auto& roi = bed_rois_[channel];

    // 2. 해당 채널의 환자 위험도 매핑 가져오기
    PatientRisk risk = getPatientRiskForChannel(channel);

    // 3. 저수준 핵심 알고리즘 호출
    detector_.update(channel, detections, roi, risk, caregiver_present);
}

// ★ 헤더의 단일 반환형(PatientRisk)에 맞춘 채널 위험도 매핑 함수
PatientRisk BedEscapeModule::getPatientRiskForChannel(int channel) {
    // [💡 실무 가이드] Database에서 이 채널(침대)에 입원한 실제 환자의 위험 등급을 조회하여 반환하면 됩니다.
    // 일단 지금 컴파일해서 테스트해 보실 수 있도록 데모 규칙(1번 채널은 상(High), 나머지는 중(Medium))을 심어둡니다.
    if (channel == 1) {
        return PatientRisk::High; 
    }
    return PatientRisk::Medium;
}