#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "detection.hpp" 

// 환자 낙상/이탈 위험도 등급
enum class PatientRisk { HIGH, MEDIUM, LOW };

class BedEgressModule {
public:
    // 알림 콜백 (침상 탈출 발생 시 호출)
    using AlarmCallback = std::function<void(int channel, int object_id)>;

    BedEgressModule() = default;

    // 콜백 등록
    void setAlarmCallback(AlarmCallback cb) { alarm_cb_ = std::move(cb); }
    
    // 위험도 설정 (Qt 클라이언트 DB 연동 시 호출)
    void setRiskLevel(int channel, PatientRisk risk);

    // 침대 ROI 업데이트 (main.cpp 배선용)
    void updateBedRoi(int channel, bool clear, const std::vector<std::pair<float, float>>& points);

    // 카메라 메타데이터(바운딩 박스) 수신 시 처리
    void processDetections(int channel, const std::vector<Detection>& dets);

private:
    // 현재 시간이 야간(22:00 ~ 06:00)인지 확인
    bool isNightTime() const;

    std::mutex mutex_;
    AlarmCallback alarm_cb_;
    
    std::map<int, PatientRisk> risk_levels_;
    std::map<int, std::vector<std::pair<float, float>>> rois_; // 채널별 ROI 저장소
    std::map<int, std::map<int, bool>> is_in_bed_; // 객체별 이전 상태 저장 (true: 침상 안, false: 밖)
};