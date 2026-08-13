#pragma once

#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "detection.hpp"

// 채널별 침대 ROI 보관소.
//
// Qt가 보낸 ROI_SET/ROI_CLEAR/ROI_BIND를 받아 들고 있다가, 판정 모듈이 매
// 메타데이터 콜백마다 zones()로 스냅샷을 떠 간다. 예전에는 모듈마다
// `std::map<int, std::vector<점>> rois_`(채널당 다각형 1개)를 각자 들고 있었는데,
// 침대가 채널당 여러 개가 되면서 자료구조·삭제 규칙이 똑같이 복잡해져 여기로 합쳤다.
//
// ★ 스레드: StreamServer 수신 스레드가 쓰고, RTSP 메타데이터 스레드와 채널별 AI
//   워커가 읽는다. zones()는 일부러 값을 복사해 돌려준다 — 판정 로직이 락을 쥔 채
//   길게 돌지 않게 하려는 것(콜백 안에서 소켓 전송까지 하는 경로가 있다).
class BedZoneStore {
public:
    // roi_id 자리에 이 값이 오면 "그 채널 전부" (프로토콜 DBJ_ROI_ID_ALL과 같은 값)
    static constexpr int kRoiIdAll = 0xFF;

    // 침대 ROI 갱신. clear=true면 삭제하고 points는 무시한다.
    // clear이면서 roi_id==kRoiIdAll이면 그 채널의 침대를 전부 지운다.
    // ★ 삭제해도 resident_id 매핑은 같이 사라진다 — 침대를 지웠는데 사람만 남으면
    //   그 사람은 영원히 귀속될 자리가 없는 유령이 된다.
    void update(int channel, int roi_id, bool clear,
                std::vector<std::pair<float, float>> points);

    // 침대 ↔ 입소자 매핑. resident_id=0이면 매핑 해제.
    // ROI를 그리기 전에 매핑이 먼저 도착해도 자리를 만들어 둔다(도착 순서 무관) —
    // 부팅 시 DB 복원에서 두 정보가 따로 들어오기 때문.
    void bind(int channel, int roi_id, int resident_id);

    // 판정용 스냅샷(값 복사). 유효하지 않은(3점 미만) 침대도 그대로 담겨 오므로,
    // 쓰는 쪽은 BedZone::valid()를 확인하거나 feetInWhichZone()을 쓸 것.
    std::map<int, BedZone> zones(int channel) const;

    // 이 채널에 판정에 쓸 수 있는(3점 이상) 침대가 하나라도 있는지
    bool hasZones(int channel) const;

private:
    mutable std::mutex mutex_;
    std::map<int, std::map<int, BedZone>> channels_;  // 채널 → (roi_id → 침대)
};
