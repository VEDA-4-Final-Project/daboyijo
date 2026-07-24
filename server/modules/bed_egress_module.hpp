#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "detection.hpp" 

class Database;

// 환자 위험도 등급(1: 하, 2: 중, 3: 상)
enum class PatientStatus {
    LOW = 1,
    MEDIUM = 2,
    HIGH = 3
};

class BedEgressModule {
public:
    // 알림 콜백 (침상 탈출 발생 시 호출)
    using AlarmCallback = std::function<void(int channel, int object_id)>;

    BedEgressModule() = default;

    // 콜백 등록
    void setAlarmCallback(AlarmCallback cb) { alarm_cb_ = std::move(cb); }
    
    // 환자 상태 실시간 업데이트 (Qt 클라이언트 제어 신호 배선용)
    void updatePatientStatus(int channel, int status);

    // 서버 부팅 시 DB로부터 기존 환자 관리 정보 초기 로드
    void initializeFromDb(Database& db);

    // 침대 ROI 업데이트 (main.cpp 배선용)
    void updateBedRoi(int channel, bool clear, const std::vector<std::pair<float, float>>& points);

    // 카메라 메타데이터(바운딩 박스) 수신 시 처리
    void processDetections(int channel, const std::vector<Detection>& dets);

private:
    // 현재 시간이 야간(22:00 ~ 06:00)인지 확인
    bool isNightTime() const;

    std::mutex mutex_;
    AlarmCallback alarm_cb_;
    
    std::map<int, PatientStatus> patient_statuses_;
    std::map<int, std::vector<std::pair<float, float>>> rois_; // 채널별 ROI 저장소
    std::map<int, std::map<int, bool>> is_in_bed_; // 객체별 이전 상태 저장 (true: 침상 안, false: 밖)

    // 경계선 흔들림 노이즈로 인한 알람 버스트(폭주) 방지용 쿨다운 타이머
    // 구조: 채널 ID -> [ 객체 ID -> 마지막 알람 발생 시각 ]
    std::map<int, std::map<int, std::chrono::steady_clock::time_point>> last_alarm_time_;
};