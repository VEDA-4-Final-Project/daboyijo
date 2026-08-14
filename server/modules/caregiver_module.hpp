#pragma once

#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "ai_worker.hpp"
#include "care_timer.hpp"
#include "caregiver_detector.hpp"
#include "database.hpp"

// ══ [요양사 감지] 모듈 — 담당자는 이 파일과 video/caregiver_detector.*,
//    video/care_timer.* 만 수정하면 된다 ══
//
// 옷 색(HSV) 기반 보호사 판정 → 채널별 케어 세션 타이머 갱신 →
// 세션 종료 시 DB(care_logs)에 케어시간 기록.
// 세션이 끝난 뒤 짧은 시간(kMergeWindowSec) 안에 요양사가 돌아오면 새 줄을 만들지
// 않고 직전 줄에 케어시간을 더한다 — 물품을 가지러 잠깐 나간 것까지 케어가 끊긴
// 것으로 세면 한 번의 케어가 여러 건으로 쪼개진다.
// 색 범위·세션 타이머 튜닝값은 caregiver_module.cpp 상단에 있다.
class CaregiverModule {
public:
    // 이 프레임에서 보호사로 판정된 객체들의 ObjectId 통보.
    // IdentityTracker가 "이 트랙은 보호사"로 표시해 침대 귀속에서 빼는 데 쓴다 —
    // 보호사가 침대에 걸터앉으면 발끝이 ROI 안이라 그대로 두면 환자로 둔갑한다.
    using CaregiverIdsCallback =
        std::function<void(int channel, const std::vector<int>& object_ids)>;

    explicit CaregiverModule(Database& db) : db_(db) {}

    // 콜백을 걸면 프레임마다 사람 전원을 검사한다(안 걸면 첫 보호사에서 멈춤).
    void setCaregiverIdsCallback(CaregiverIdsCallback cb) { ids_cb_ = std::move(cb); }

    // 채널 등록 (AiWorker start 전, 카메라 루프에서 호출)
    void addChannel(int channel);

    // AiWorker에 등록할 프로세서 — 프레임에서 보호사 존재 판정 + 타이머 갱신
    void processFrame(const AiJob& job);

    // 종료 시 열린 케어 세션 마감 (기록 유실 방지)
    void flush();

private:
    // 색 판정기 — 내부 상태가 없어(읽기 전용 파라미터뿐) 채널별 워커
    // 스레드들이 동시에 호출해도 안전.
    CaregiverDetector detector_;
    // 채널별 케어 타이머. addChannel은 메인 스레드에서 AI 워커 시작 전에만
    // 호출되고, 이후 각 항목은 그 채널의 워커 스레드 전용이라 락 불필요.
    std::map<int, CareTimer> timers_;
    // 채널별로 마지막에 기록한 care_logs.log_id (0 = 아직 없음/기록 실패).
    // 요양사가 잠깐 자리를 비웠다 돌아오면 새 행 대신 이 행에 이어붙인다.
    // ★ timers_ 와 같은 규칙 — addChannel 에서 미리 채워 런타임 삽입이 없게
    //   하므로 각 항목은 그 채널의 워커 스레드 전용이고 락이 필요 없다.
    std::map<int, long long> lastLogId_;
    // 보호사 ObjectId 통보 콜백. 등록은 AI 워커 시작 전 1회뿐이라 락이 필요 없다.
    CaregiverIdsCallback ids_cb_;
    Database& db_;  // insertCareLog는 내부 뮤텍스로 보호됨 (database.hpp)
};
