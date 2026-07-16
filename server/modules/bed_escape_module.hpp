#pragma once

#include <vector>
#include <unordered_map>
#include <mutex>
#include <utility>
#include <functional>

#include "detection.hpp"
#include "bed_escape_detector.hpp"

class BedEscapeModule {
public:
    using EscapeCallback = std::function<void(int channel, const Detection& detection)>;

    // 기본 생성자 (필요 시 DB 연동을 위해 의존성 주입 가능)
    BedEscapeModule();

    // main.cpp 조립도 배선용 인터페이스들
    void setEscapeCallback(EscapeCallback cb);
    void updateBedRoi(int channel, bool clear, const std::vector<std::pair<float, float>>& points);
    void onMetadata(int channel, const std::vector<Detection>& detections, bool caregiver_present);

private:
    // 채널별 환자 위험도(상/중/하)를 조회하는 함수
    PatientRisk getPatientRiskForChannel(int channel);

    std::mutex mutex_;                       // 멀티스레드(RTSP 수신 스레드 등) 보호용 락
    BedEscapeDetector detector_;             // 저수준 Core 알고리즘 인스턴스
    EscapeCallback on_escape_ = nullptr;     // main.cpp로 이벤트를 쏠 콜백

    // 채널별로 저장해 둘 침대 ROI 맵
    std::unordered_map<int, std::vector<std::pair<float, float>>> bed_rois_;
};