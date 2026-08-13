#include "activity_module.hpp"

#include <chrono>
#include <cstdio>

#include "database.hpp"

namespace {
// 한 패킷 사이에 늘어날 수 있는 걸음 수의 상한. 초 단위로 올라오는데 이보다
// 많이 뛰면 센서 오작동이나 값 튐으로 본다(사람이 1초에 20걸음을 걷진 않는다).
// 걸러내지 않으면 튄 값 하나가 그날 활동량을 통째로 망친다.
constexpr int kMaxStepsPerPacket = 20;

// 지금 시각이 속한 "분" 의 시작(epoch 초).
// ★ 패킷 안의 timestamp 를 쓰지 않고 서버 시계를 쓴다. 릴레이가 자기 시계로
//   찍은 값이라 파이의 시각이 틀어져 있으면 리포트의 시간대가 통째로 밀린다.
//   care_logs·events 가 전부 서버의 NOW() 기준이라 거기에 맞춘다.
long long currentMinuteEpoch() {
    const auto now = std::chrono::system_clock::now();
    const long long sec = std::chrono::duration_cast<std::chrono::seconds>(
                              now.time_since_epoch()).count();
    return sec - (sec % 60);
}
}  // namespace

void ActivityModule::onWearableData(const WearableData& data) {
    if (data.device_id.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    Acc& a = acc_[data.device_id];

    const long long minute = currentMinuteEpoch();

    // ── 분이 바뀌었으면 직전 1분치를 먼저 내보낸다 ──
    if (a.minute >= 0 && minute != a.minute) {
        flushLocked(data.device_id, a);
    }
    if (a.minute != minute) {
        a.minute = minute;
        // 입소자 매핑을 분마다 다시 확인한다. Qt 에서 웨어러블을 다른 사람에게
        // 옮겨 채워도 1분 안에 따라간다. 웨어러블은 사람마다 다르므로 카메라와
        // 달리 여기엔 모호함이 없다.
        a.resident_id = db_.getResidentByWearable(data.device_id);
    }

    // ── 걸음 수: 누적값이라 차분해야 한다 ──
    if (a.prev_steps >= 0) {
        int d = data.steps - a.prev_steps;
        if (d < 0) {
            // 웨어러블 재부팅(0으로 리셋) 또는 2바이트 롤오버(65535→0).
            // 여기서 0으로 눌러주지 않으면 그날 활동량이 -65000 이나 65000 으로 찍힌다.
            std::fprintf(stderr, "[Activity] %s 걸음수 역행 (%d → %d) — 리셋으로 보고 0 처리\n",
                         data.device_id.c_str(), a.prev_steps, data.steps);
            d = 0;
        } else if (d > kMaxStepsPerPacket) {
            std::fprintf(stderr, "[Activity] %s 걸음수 급증 (+%d) — 값 튐으로 보고 %d 로 제한\n",
                         data.device_id.c_str(), d, kMaxStepsPerPacket);
            d = kMaxStepsPerPacket;
        }
        a.steps_delta += d;
    }
    a.prev_steps = data.steps;
    a.last_cum   = data.steps;

    // ── 생체값: 0 은 "미착용/측정실패" 지 "0bpm" 이 아니다(MqttMasterManager 와 같은 규칙).
    //    평균에 섞으면 값이 통째로 내려앉으므로 뺀다.
    if (data.heart_rate > 0) { a.hr_sum   += data.heart_rate; ++a.hr_n; }
    if (data.spo2       > 0) { a.spo2_sum += data.spo2;       ++a.spo2_n; }
}

void ActivityModule::flushLocked(const std::string& device_id, Acc& a) {
    if (a.minute < 0) return;

    if (a.resident_id < 0) {
        // residents.wearable_id 에 이 기기가 등록되지 않았다. 누구 것인지 모르는
        // 활동량은 리포트에 쓸 수 없으므로 버리되, 조용히 버리지는 않는다.
        std::fprintf(stderr, "[Activity] ⚠ %s 에 매핑된 입소자 없음 — 1분치 버림"
                             " (residents.wearable_id 확인 필요)\n", device_id.c_str());
    } else {
        const int hr   = a.hr_n   > 0 ? static_cast<int>(a.hr_sum   / a.hr_n)   : 0;
        const int spo2 = a.spo2_n > 0 ? static_cast<int>(a.spo2_sum / a.spo2_n) : 0;
        db_.upsertActivityMinute(a.resident_id, a.minute, a.steps_delta, a.last_cum, hr, spo2);
    }

    // prev_steps 는 일부러 남긴다 — 다음 분의 첫 차분이 이 값에서 이어져야
    // 분 경계에서 걸음이 새지 않는다.
    a.steps_delta = 0;
    a.hr_sum = a.spo2_sum = 0;
    a.hr_n = a.spo2_n = 0;
    a.minute = -1;
}

void ActivityModule::flushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [device_id, a] : acc_) flushLocked(device_id, a);
}
