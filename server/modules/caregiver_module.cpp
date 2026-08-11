#include "caregiver_module.hpp"

#include <cstdio>

namespace {

// ── 케어 세션 튜닝값 ─────────────────────────────────────────────
// 보호사가 kAbsentTimeoutSec초 이상 안 보이면 세션 종료,
// kMinSessionSec초 미만 세션은 오탐으로 보고 버린다.
constexpr double kAbsentTimeoutSec = 3.0;
constexpr double kMinSessionSec = 5.0;
// 세션이 끝난 뒤 이 시간 안에 요양사가 돌아오면 "잠깐 자리를 비운 것"으로 보고
// 새 기록을 만들지 않고 직전 기록에 케어시간을 더한다. 기저귀 갈 물품을 가지러
// 가거나 옆 방을 잠깐 보고 오는 정도는 케어가 끊긴 게 아니라고 판단.
// ★ 비운 시간 자체는 케어시간에 안 들어간다 — 세션마다 감지된 구간만 더한다.
constexpr double kMergeWindowSec = 180.0;   // 3분
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

    // 병합용 log_id 자리를 여기서 미리 만들어 둔다 — 워커 스레드들이 도는 중에
    // map 에 삽입이 일어나지 않게 해서 락 없이 쓰기 위함(헤더 주석 참고).
    lastLogId_[channel] = 0;

    result.first->second.onSessionEnd([channel, this](int dur, double gap) {
        std::fprintf(stderr, "[ch%d] 케어 세션 종료: %d초\n", channel + 1, dur);

        long long& lastId = lastLogId_[channel];

        // 직전 기록이 있고 그 뒤 공백이 3분 이내면 같은 행에 이어붙인다.
        // log_id 를 그대로 들고 있으므로 계속 들락거려도 한 행에 누적된다.
        if (lastId > 0 && gap >= 0.0 && gap <= kMergeWindowSec
            && db_.addCareLogDuration(lastId, dur)) {
            std::fprintf(stderr, "[ch%d] 자리 비움 %.0f초 → 직전 케어에 합산\n",
                         channel + 1, gap);
            return;
        }

        // 새 케어이거나, 병합하려다 실패한 경우(대상 행이 없거나 DB 오류).
        // 실패했을 때 그냥 버리면 케어시간이 사라지므로 새 행으로 남긴다.
        lastId = db_.insertCareLog(channel, dur);   // 실패 시 0 — 다음 병합은 자동 포기
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