#include "caregiver_module.hpp"

#include <cstdio>

namespace {

// ── 케어 세션 튜닝값 ─────────────────────────────────────────────
// 보호사가 kAbsentTimeoutSec초 이상 안 보이면 세션 종료,
// kMinSessionSec초 미만 세션은 오탐으로 보고 버린다.
constexpr double kAbsentTimeoutSec = 3.0;
constexpr double kMinSessionSec = 5.0;
// ★ 실측 근거: 조끼 S 146~207 / 비조끼 S 55~130.
//   조끼가 움직이면 모션 블러로 채도가 146까지 떨어지므로 여유를 둬 140으로 컷.
//   (비조끼도 드물게 148~167로 튀는 프레임이 있어 완전한 분리는 아니다 —
//    그래서 ratio 조건과 AND로 묶어 두 축을 같이 본다.)
constexpr double kMinSaturation = 140.0;

// ── 요양사 유니폼 색(HSV) 튜닝값 ──────────────────────────────────
// 카메라 화질이 뿌옇고 대비가 낮아 실제 조끼 픽셀의 S/V가 낮게 깔린다.
// 헤더 기본값(S>=100, V>=80)은 대부분을 탈락시켜서 하한을 크게 내렸다.
// ★ 이 범위는 "따뜻한 색 전부"라 피부·나무까지 통과시킨다. 범위를 조이는 대신
//   위 kMinSaturation으로 걸러내는 방식을 택했다 (범위를 좁히면 조명이 바뀔 때
//   조끼 자체를 놓치는 쪽이 더 위험하다고 판단).
const cv::Scalar kVestLower(5, 50, 50);
const cv::Scalar kVestUpper(28, 255, 255);
// ★ 채도가 주 판정을 맡으므로 ratio는 하한 보증 역할만 한다.
//   기존 0.08은 서 있는 요양사를 놓쳤고(실측 0.028~0.073 탈락), 반대로 완전히
//   풀면 비조끼가 S 165로 튀는 프레임(그때 ratio 0.071)이 통과해 헛세션이 열린다.
constexpr double kVestThreshold = 0.05;

}  // namespace

void CaregiverModule::addChannel(int channel) {
    // 색 범위·임계값 주입 (중복 호출돼도 같은 값이라 무해)
    detector_.setColorRange(kVestLower, kVestUpper);
    detector_.setThreshold(kVestThreshold);
    detector_.setMinSaturation(kMinSaturation);

    // 카메라 채널별 careTimer 등록
    auto result = timers_.emplace(
        channel, CareTimer(kAbsentTimeoutSec, kMinSessionSec));
    if (!result.second) return;  // 이미 등록됨

    // timer에 채널 번호 주입
    result.first->second.setChannel(channel);

    result.first->second.onSessionEnd([channel, this](int dur) {
        std::fprintf(stderr, "[ch%d] 케어 세션 종료: %d초\n", channel + 1, dur);
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