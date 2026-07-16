#include "caregiver_module.hpp"

#include <cstdio>
#include <mutex>

namespace {

// ── 케어 세션 튜닝값 ─────────────────────────────────────────────
// 보호사가 kAbsentTimeoutSec초 이상 안 보이면 세션 종료,
// kMinSessionSec초 미만 세션은 오탐으로 보고 버린다.
constexpr double kAbsentTimeoutSec = 3.0;
constexpr double kMinSessionSec = 5.0;

}  // namespace

void CaregiverModule::addChannel(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = timers_.emplace(
        channel, CareTimer(kAbsentTimeoutSec, kMinSessionSec));
    if (!result.second) return;  // 이미 등록됨

    result.first->second.onSessionEnd([channel, this](int dur) {
        std::printf("[ch%d] 케어 세션 종료: %d초\n", channel, dur);
        db_.insertCareLog(channel, dur);
    });
}

void CaregiverModule::processFrame(const AiJob& job) {
    DetectionFrame df;
    df.channel = job.channel;
    df.objects = job.dets;
    // 색 비율 판정이라 축소 이미지로 충분 — 가벼운 small_frame 사용
    bool present = detector_.detectInFrame(job.small_frame, df);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timers_.find(job.channel);
    if (it != timers_.end()) it->second.update(present);
}

void CaregiverModule::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : timers_) {
        entry.second.flush();
    }
}

bool CaregiverModule::isCaregiverActive(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timers_.find(channel);
    if (it != timers_.end()) {
        return it->second.inSession(); // 현재 케어 세션이 켜져 있는지 여부 반환
    }
    return false;
}
