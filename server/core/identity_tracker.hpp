#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include "detection.hpp"

// 추적 ID(object_id) ↔ 침대(roi_id) ↔ 입소자(resident_id) 귀속기.
//
// ── 왜 필요한가 ────────────────────────────────────────────────
// WiseAI가 주는 object_id는 "프레임 간 같은 덩어리"라는 뜻일 뿐 신원이 아니다.
// 사람이 가려졌다 다시 보이면 같은 사람인데 새 ID가 붙는다. 그래서 ID만으로는
// 낙상·이탈 알림에 "누가"를 붙일 수 없고, 지금까지는 obj=7 같은 숫자만 남았다.
//
// 얼굴 인식은 프라이버시 정책(전원 블러)상 못 쓴다. 대신 요양원은 조건이 좋다 —
// 침대가 고정이고, 입소자는 자기 침대에서 대부분의 시간을 보낸다. 그래서 공간
// (침대 ROI)을 앵커로 삼아 트랙에 사람을 건다:
//
//   ① 귀속  — 새 트랙이 처음 보인 자리가 침대 안이면 그 침대 입소자로 라벨링
//   ② 유지  — 침대를 나가도 라벨을 유지한다(sticky). 나갔다고 라벨을 지우면
//             정작 화장실 가다 넘어졌을 때 이름을 못 붙인다. 대신 침대 밖에
//             오래 있을수록 신뢰도를 깎는다.
//   ③ 인계  — 트랙이 사라진 뒤 kHandoverSec 안에 그 자리 근처에서 새 ID가 뜨면
//             같은 사람으로 보고 라벨을 물려준다(ID 스위치 보정)
//   ④ 제외  — 요양보호사로 판정된 트랙은 침대에 들어가도 귀속하지 않는다.
//             보호사가 침대에 걸터앉으면 발끝이 ROI 안으로 들어와, 그대로 두면
//             보호사가 그 침대 환자로 둔갑한다.
//
// ── 한계 (그래서 confidence를 들고 다닌다) ──────────────────────
// 두 사람이 겹쳐 지나가면 트래커가 ID를 맞바꿀 수 있고 라벨도 같이 뒤바뀐다.
// 침대가 붙어 있을수록 위험하다. 이걸 없앨 방법이 없으므로 "확정" 대신
// 0~1 신뢰도를 같이 내보내고, kMinNameConfidence 아래면 호출부가 이름 대신
// "미상"으로 알리게 한다. 잘못된 이름은 이름 없는 것보다 나쁘다.
class IdentityTracker {
public:
    // 이 값 미만이면 이름을 붙이지 않는다 (알림에 "미상"으로 나감)
    static constexpr float kMinNameConfidence = 0.5f;

    struct Identity {
        int roi_id = kNoZone;    // 귀속된 침대 (kNoZone = 미상)
        int resident_id = 0;     // 귀속된 입소자 (0 = 미상)
        float confidence = 0.f;  // 이 라벨이 맞을 것이라는 정도 0~1
        bool caregiver = false;  // 요양보호사로 판정된 트랙

        // 알림에 실명을 쓸 만큼 믿을 수 있는가
        bool named() const {
            return resident_id > 0 && confidence >= kMinNameConfidence;
        }
    };

    // 메타데이터 콜백마다 1회 (RTSP 수신 스레드).
    // zones는 그 채널의 최신 침대 목록 — BedZoneStore::zones()로 뜬 스냅샷.
    void update(int channel, const std::vector<Detection>& dets,
                const std::map<int, BedZone>& zones);

    // 지금 이 트랙이 누구인지. 모르면 resident_id=0, confidence=0.
    Identity identify(int channel, int object_id) const;

    // 요양보호사로 판정된 객체들을 알려준다 (AI 워커 스레드에서 프레임마다).
    // ★ 한 번 보호사로 찍힌 트랙은 계속 보호사로 둔다 — 조끼는 자세·조명에 따라
    //   프레임마다 놓칠 수 있는데, 놓친 프레임에 침대로 들어가면 그 즉시 환자로
    //   둔갑해 버린다. 놓치는 쪽보다 붙잡아 두는 쪽이 오귀속을 덜 만든다.
    void markCaregivers(int channel, const std::vector<int>& object_ids);

private:
    struct Trace {
        int roi_id = kNoZone;
        int resident_id = 0;
        float confidence = 0.f;
        bool caregiver = false;
        float x = 0, y = 0;  // 마지막 발끝 위치(정규화) — 인계 후보 거리 계산용
        std::chrono::steady_clock::time_point last_seen;
        // 화면에서 사라짐. 바로 지우지 않고 잠시 "그림자"로 남겨 둔다 —
        // 같은 사람이 새 ID로 다시 뜰 때 라벨을 물려주기 위한 것.
        bool gone = false;
    };

    mutable std::mutex mutex_;
    std::map<int, std::map<int, Trace>> channels_;  // 채널 → (object_id → 상태)
};
