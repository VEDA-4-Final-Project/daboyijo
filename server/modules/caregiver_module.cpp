#include "caregiver_module.hpp"

#include <cstdio>

namespace {

// ── 케어 세션 튜닝값 ─────────────────────────────────────────────
// 보호사가 kAbsentTimeoutSec초 이상 안 보이면 세션 종료,
// kMinSessionSec초 미만 세션은 오탐으로 보고 버린다.
constexpr double kAbsentTimeoutSec = 3.0;
constexpr double kMinSessionSec = 5.0;

// ── 요양사 유니폼 색(HSV) 튜닝값 ──────────────────────────────────
// 카메라 화질이 뿌옇고 대비가 낮아 실제 조끼 픽셀의 S/V가 낮게 깔린다.
// 헤더 기본값(S>=100, V>=80)은 대부분을 탈락시켜서 하한을 크게 내렸다.
// 느슨하게 잡아 마스크가 잡히는지 먼저 확인 → 실측 후 다시 조인다.
const cv::Scalar kVestLower(5, 50, 50);
const cv::Scalar kVestUpper(28, 255, 255);
constexpr double kVestThreshold = 0.08;

}  // namespace

void CaregiverModule::addChannel(int channel) {
    // 색 범위·임계값 주입 (중복 호출돼도 같은 값이라 무해)
    detector_.setColorRange(kVestLower, kVestUpper);
    detector_.setThreshold(kVestThreshold);

    //카메라 채널별 careTimer 등록
    auto result = timers_.emplace(
        channel, CareTimer(kAbsentTimeoutSec, kMinSessionSec));
    if (!result.second) return;  // 이미 등록됨

    //result.first->first: channel (채널 번호)
    //result.first->second: Caretiemr 객체
    //[channel, this]로 현재 채널과 caregiverModule 객체 기억
    //DB에 케어 시간 저장하는 콜백 등록
    result.first->second.onSessionEnd([channel, this](int dur) {
        std::printf("[ch%d] 케어 세션 종료: %d초\n", channel + 1, dur);
        db_.insertCareLog(channel, dur);
    });
}


void CaregiverModule::processFrame(const AiJob& job) {
    //DetectionFrame 형식의 df 생성
    DetectionFrame df;
    df.channel = job.channel;
    df.objects = job.dets;

    // 매 프레임마다 raw_frame과 df(채널번호, 탐지된 객체 목록)을 전달하여
    // detector가 detectInFrame을 실행하여 요양사 탐지
    bool present = detector_.detectInFrame(job.raw_frame, df);

    //현재 카메라의 CareTimer찾음
    auto it = timers_.find(job.channel);
    //찾은 CareTimer에게 탐지 결과 전달
    if (it != timers_.end()) it->second.update(present);
}

//열려있는 세션들 강제 종료
void CaregiverModule::flush() {
    for (auto& entry : timers_) {
        entry.second.flush();
    }
}