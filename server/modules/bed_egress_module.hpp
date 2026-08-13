#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "bed_zones.hpp"
#include "detection.hpp"

class Database;

// 환자 위험도 등급(1: 하, 2: 중, 3: 상)
enum class PatientStatus {
    LOW = 1,
    MEDIUM = 2,
    HIGH = 3
};

// 침상 이탈 1건. 채널만으로는 "누가" 나갔는지 알 수 없어서(한 채널에 침대가
// 여럿) 어느 침대에서 누가 나갔는지까지 실어 보낸다.
struct EgressEvent {
    int channel = 0;
    int object_id = 0;     // WiseAI 추적 ID (같은 사람이라도 재등장하면 바뀜)
    int roi_id = kNoZone;  // 이탈한 침대
    int resident_id = 0;   // 그 침대에 매핑된 입소자 (0 = 미지정)
    float x = 0, y = 0;    // 이탈 시점 발끝 위치(정규화 0~1) — Qt 화면 마커용
};

class BedEgressModule {
public:
    // 알림 콜백 (침상 탈출 발생 시 호출)
    using AlarmCallback = std::function<void(const EgressEvent&)>;

    BedEgressModule() = default;

    // 콜백 등록
    void setAlarmCallback(AlarmCallback cb) { alarm_cb_ = std::move(cb); }

    // 환자 위험도 실시간 업데이트 (Qt 클라이언트 제어 신호 배선용).
    // 위험도는 사람에게 붙는 값이라 채널이 아니라 침대 단위다.
    // roi_id == BedZoneStore::kRoiIdAll 이면 그 채널의 모든 침대에 일괄 적용.
    void updatePatientStatus(int channel, int roi_id, int status);

    // 서버 부팅 시 DB로부터 기존 환자 관리 정보 초기 로드.
    // ★ 침대 ROI가 먼저 복원돼 있어야 한다(main.cpp가 roi_zones를 읽어
    //   BedZoneStore에 밀어 넣은 뒤에 호출할 것) — 위험도를 침대에 매핑된
    //   입소자에게서 읽기 때문.
    void initializeFromDb(Database& db);

    // 침대 ROI 보관소 연결 (main.cpp가 소유, 여기서는 읽기만).
    // ROI는 낙상·침상이탈·객체 귀속이 같은 값을 봐야 해서 한 곳에만 둔다.
    void setBedZones(const BedZoneStore* zones) { zones_ = zones; }

    // 침대가 삭제됐을 때 그 침대에 매달린 상태(위험도·재실·쿨다운)를 같이 비운다.
    // 안 그러면 나중에 같은 roi_id로 다른 침대를 그렸을 때 옛 상태를 물려받는다.
    // roi_id == BedZoneStore::kRoiIdAll 이면 그 채널 전체.
    void resetZoneState(int channel, int roi_id);

    // 카메라 메타데이터(바운딩 박스) 수신 시 처리
    void processDetections(int channel, const std::vector<Detection>& dets);

private:
    // 현재 시간이 야간(22:00 ~ 06:00)인지 확인
    bool isNightTime() const;
    // 이 침대의 위험도. 등록된 값이 없으면 환자 안전을 위해 HIGH.
    PatientStatus statusOf(int channel, int roi_id) const;  // mutex_ 를 쥔 채 호출할 것

    mutable std::mutex mutex_;
    AlarmCallback alarm_cb_;

    const BedZoneStore* zones_ = nullptr;  // main.cpp 소유 (자체 락 보유)

    // 침대별 위험도. 구조: 채널 → [ roi_id → 위험도 ]
    // 채널 단위였을 땐 한 채널에 여러 명이 있어도 등급이 하나뿐이라, 위험도 '하'인
    // 사람 때문에 같은 방 '상'인 사람의 이탈까지 묻히는 문제가 있었다.
    std::map<int, std::map<int, PatientStatus>> patient_statuses_;

    // 채널 일괄(kRoiIdAll)로 들어온 위험도. 아직 침대에 매핑되지 않은 입소자의
    // 위험도가 여기로 온다 — 침대별 값이 없을 때의 폴백이다. 이게 없으면 '하'로
    // 지정해도 침대 매핑 전에는 값이 안 남아 기본값 '상'으로 계속 울린다.
    std::map<int, PatientStatus> channel_default_;

    // 객체별 직전 위치. 값은 그때 있던 침대의 roi_id (kNoZone = 침대 밖).
    // bool(침상 안/밖)로는 침대 A에서 침대 B로 옮겨간 것과 그냥 머문 것을
    // 구분할 수 없어 침대 번호를 그대로 들고 있는다.
    std::map<int, std::map<int, int>> last_zone_;

    // 경계선 흔들림 노이즈로 인한 알람 버스트(폭주) 방지용 쿨다운 타이머
    // 구조: 채널 ID -> [ 객체 ID -> 마지막 알람 발생 시각 ]
    std::map<int, std::map<int, std::chrono::steady_clock::time_point>> last_alarm_time_;
};
