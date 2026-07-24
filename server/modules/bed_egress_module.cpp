#include "bed_egress_module.hpp"
#include "database.hpp"

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <chrono>

void BedEgressModule::updatePatientStatus(int channel, int status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status >= 1 && status <= 3) {
        patient_statuses_[channel] = static_cast<PatientStatus>(status);
        std::printf("[BedEgress] 채널 %d 환자 관리 상태 실시간 갱신: %d\n", channel, status);
    }
}

// 서버 구동 시 초기 데이터베이스 연동 및 환자 상태 동기화
void BedEgressModule::initializeFromDb(Database& db) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 위험도의 진짜 소스는 residents.risk_level(Qt가 저장 시 기록하는 곳)이다.
    // 재시작해도 여기서 복원되므로, Qt에서 바꾼 '하'가 부팅 후에도 유지된다.
    for (int ch = 0; ch < 4; ++ch) {
        int level = db.getRiskLevelByCamera(ch);
        if (level >= 1 && level <= 3) {
            patient_statuses_[ch] = static_cast<PatientStatus>(level);
        } else {
            // 이 채널에 재원 입소자가 없거나 조회 실패 → 환자 안전을 위해 기본값 최상위(HIGH)
            patient_statuses_[ch] = PatientStatus::HIGH;
        }

        // 버퍼링에 묻히지 않도록 stderr로 — 부팅 시 각 채널이 무슨 등급으로 떴는지 바로 보이게
        const char* label = (patient_statuses_[ch] == PatientStatus::HIGH)   ? "상"
                          : (patient_statuses_[ch] == PatientStatus::MEDIUM) ? "중" : "하";
        std::fprintf(stderr, "[BedEgress] 부팅 위험도 로드: ch%d = %s\n", ch, label);
    }
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
    PatientStatus status = PatientStatus::HIGH; // 안전을 위해 기본값 '상'

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rois_.find(channel);
        if (it == rois_.end() || it->second.size() < 3) return; 
        roi = it->second;

        if (patient_statuses_.count(channel)) {
            status = patient_statuses_[channel];
        }
    }

    // ── [환자 관리 상태별 알림 필터링 구조] ──
    // ⚠️ 조기 return하면 맨 아래 [메모리 관리]가 마비되므로, 플래그 변수 처리로 변경합니다.
    bool alarm_enabled = true;

    if (status == PatientStatus::LOW) {
        alarm_enabled = false; // 하 (1) : 이탈해도 알림을 주지 않음
    } 
    else if (status == PatientStatus::MEDIUM && !isNightTime()) {
        alarm_enabled = false; // 중 (2) : 야간(22시~06시)이 아닐 때는 알림을 차단
    }

    // ── [침상 탈출 감지 로직] ──
    std::vector<int> current_obj_ids;
    std::vector<int> trigger_ids; // 락 밖에서 안전하게 콜백을 쏘기 위한 임시 보관함
    auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& det : dets) {
            // 사람 객체만 검사
            if (!det.isHuman()) continue;
            current_obj_ids.push_back(det.object_id);

            // detection.hpp에 정의된 공용 헬퍼 함수 활용
            bool is_currently_in_bed = isFeetInRoi(det, roi);
            
            // 이 객체 ID를 시스템이 처음 본 경우 상태 등록 후 통과
            if (!is_in_bed_[channel].count(det.object_id)) {
                is_in_bed_[channel][det.object_id] = is_currently_in_bed;
                continue; 
            }

            bool was_in_bed = is_in_bed_[channel][det.object_id];

            // 이전 프레임에는 침상 안에 있었는데, 이번 프레임에서 밖으로 나갔다면
            if (was_in_bed && !is_currently_in_bed) {
                if (alarm_enabled) {
                    // [디바운스/쿨다운 타임 도입] 
                    // 경계선 좌표 흔들림 노이즈로 인해 1초에 알람 수십 번 연타되는 현상 방지 (10초 쿨다운)
                    auto& last_time = last_alarm_time_[channel][det.object_id];
                    if (std::chrono::duration<double>(now - last_time).count() >= 10.0) {
                        trigger_ids.push_back(det.object_id); // 보관만 해둠
                        last_time = now;
                    }
                }
            }
            
            // 현재 상태를 다음 프레임 비교를 위해 저장
            is_in_bed_[channel][det.object_id] = is_currently_in_bed;
        }

        // ── [메모리 관리 - 누수 방지] ──
        // 알림 차단 여부와 무관계하게 무조건 실행되어 화면에서 사라진 ID 제거
        for (auto it = is_in_bed_[channel].begin(); it != is_in_bed_[channel].end(); ) {
            if (std::find(current_obj_ids.begin(), current_obj_ids.end(), it->first) == current_obj_ids.end()) {
                last_alarm_time_[channel].erase(it->first); // 쿨다운 맵도 함께 청소
                it = is_in_bed_[channel].erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── [데드락 방지 - 안전 지대] ──
    // 락이 완전히 풀린 상태이므로 여기서 소켓 전송/블랙박스 연동을 해도 절대 서버가 멈추지 않습니다.
    for (int obj_id : trigger_ids) {
        const char* status_str = (status == PatientStatus::HIGH) ? "🔴 상(즉시 경보)" : "🟠 중(야간 관찰)";
        std::fprintf(stderr, "⚠️ [ch%d] [%s] 환자 침상 탈출 발생! (obj: %d)\n", 
                     channel, status_str, obj_id);

        if (alarm_cb_) {
            alarm_cb_(channel, obj_id); 
        }
    }
}