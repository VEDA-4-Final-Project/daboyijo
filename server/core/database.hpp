#ifndef DATABASE_HPP
#define DATABASE_HPP
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <mysql.h>   // libmariadb-dev 제공

// 일일 리포트용 이벤트 분류. 문자열 대신 열거형으로 받는 이유:
// events.event_type / source 가 ENUM 컬럼이라 오타가 나면 INSERT 가 통째로
// 실패한다. 여기서 막으면 호출부가 오타를 낼 수 없다.
enum class EventType   { Fall, BedEgress, VitalAbnormal };
enum class EventSource { Camera, Wearable };   // 같은 낙상이라도 근거가 다르다

// roi_zones 한 줄 — 침대 ROI 1개. 서버 부팅 시 복원용.
struct RoiZoneRow {
    int camera_id = 0;
    int roi_id = 0;
    int resident_id = 0;   // 0 = 입소자 미지정 (컬럼이 NULL인 경우)
    std::vector<std::pair<float, float>> points;  // 정규화 0~1 다각형
};

// DB로 통하는 유일한 문. DB 접속과 SQL은 전부 여기로만.
// 채널별 AI 워커 스레드들이 동시에 기록할 수 있어 커넥션을 뮤텍스로 보호한다
// (MYSQL* 하나를 여러 스레드가 동시에 쓰면 안 됨).
class Database {
public:
    Database();
    ~Database();
    bool connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& dbname,
                 unsigned int port = 3306);
    // 케어 세션 한 건 기록: 카메라 채널 + 케어시간(초). 아무 스레드나 호출 가능.
    //
    // ★ 그 채널의 재원자 "전원"에게 각각 한 행씩 만든다(2인실이면 2행).
    //   영상만으로는 요양사가 둘 중 누구를 돌봤는지 알 수 없다 — 억지로 한 명을
    //   고르면 절반이 조용히 틀린다. "요양사가 이 방에 N초 있었다"는 검증 가능한
    //   사실이므로 방 안 전원에게 같은 시간을 남기고, 해석은 리포트가 한다.
    //   (재원자가 하나도 없으면 resident_id = NULL 로 한 행만 남긴다 — 케어가
    //    일어난 사실 자체는 지워지면 안 된다)
    //
    // 반환: 새로 만들어진 log_id 목록(사람 수만큼). 실패 시 빈 벡터.
    //       이 id 들을 들고 있어야 요양사가 잠깐 자리를 비웠다 돌아왔을 때
    //       아래 addCareLogDuration 으로 같은 행들에 이어붙인다.
    std::vector<long long> insertCareLogs(int cameraId, int durationSec);
    // 기존 케어로그들에 케어시간을 더한다(자리 비움 후 복귀 → 세션 병합).
    // duration_sec 는 SQL 안에서 더한다 — 읽어서 계산해 쓰면 그 사이에 끼어들 수 있다.
    // start_time 은 그대로 두고 end_time 만 지금으로 민다. 그래서 이 행은
    // end_time - start_time > duration_sec 가 된다(그 차이가 자리 비운 시간).
    // 한 행이라도 병합에 실패하면 false — 호출자는 새 행으로 남기면 된다.
    bool addCareLogDuration(const std::vector<long long>& logIds, int addSec);
    // 환자 위험도의 유일한 소스는 residents.risk_level(Qt가 직접 기록)이다.
    // 과거 patient_status 테이블 경로(get/updatePatientStatus)는 아무도 INSERT하지
    // 않아 항상 비어 있던 죽은 경로라 제거함 — 아래 getRiskLevelByCamera로 통일.
    // residents.risk_level(상/중/하)에서 카메라 채널별 위험도를 읽는다(부팅 복원용).
    // 한 채널에 재원 입소자가 여럿이면 가장 높은 위험도(가장 안전한 쪽)를 반환.
    // 반환: 3(상)/2(중)/1(하), 재원 입소자가 없거나 실패 시 -1.
    int getRiskLevelByCamera(int channel);
    // 침대에 매핑된 입소자 1명의 위험도. 침대마다 사람이 다르므로 위험도 조회도
    // 사람 단위여야 한다(채널 단위 MAX 는 같은 방 '하'인 사람까지 '상'으로 올린다).
    // 반환: 3(상)/2(중)/1(하), 그런 입소자가 없거나 실패 시 -1.
    int getRiskLevelByResident(int residentId);
    // 입소자 이름 (알림 문구용). 없으면 빈 문자열.
    std::string getResidentName(int residentId);
    // 입소자의 호실 (MQTT 알림 노드 라우팅용). 없으면 -1.
    int getRoomByResident(int residentId);

    // ── 침대 ROI 영속화 (roi_zones) ──────────────────────────────
    // Qt가 그린 ROI는 서버 메모리에만 있어서 서버를 재시작하면 사라졌다. 침대에
    // 입소자까지 매핑하는 지금은 재시작마다 관리자가 다시 그릴 수 없으므로 DB에 남긴다.
    // points 는 정규화 0~1 다각형. 같은 (camera_id, roi_id)면 덮어쓴다.
    bool saveRoiZone(int cameraId, int roiId,
                     const std::vector<std::pair<float, float>>& points);
    // 침대 ↔ 입소자 매핑만 갱신 (ROI 좌표는 그대로). residentId=0 이면 NULL로 해제.
    bool bindRoiZoneResident(int cameraId, int roiId, int residentId);
    // 침대 1개 삭제. roiId < 0 이면 그 채널 전부 삭제.
    bool deleteRoiZone(int cameraId, int roiId);
    // 전 채널의 침대 ROI를 읽어온다 (부팅 복원용). 실패 시 빈 벡터.
    std::vector<RoiZoneRow> loadRoiZones();
    // 웨어러블 device_id("wearable_01" 같은 문자열)로 조회한다.
    // 릴레이가 보내는 WearableData.device_id 가 그대로 들어오며,
    // residents.wearable_id 컬럼과 대응한다. 없으면 -1.
    int getCHById(const std::string& wearable_id);
    int getRoomById(const std::string& wearable_id);
    int getRoomByCh(int channel);

    // ── 일일 리포트용 기록 (report_schema.sql) ────────────────────
    // 기록 주체는 전부 서버다. Qt 관제 앱은 읽기만 한다 — 관제 PC 는 퇴근하면
    // 꺼지는데 낙상·침상이탈은 밤에 제일 많이 나서, Qt 가 쓰면 리포트의 밤
    // 시간대가 통째로 빈다.

    // 카메라 채널의 재원 입소자 id. 없으면 -1.
    //
    // ★ 1채널 = 1인이 전제다. 카메라는 "누가" 넘어졌는지 모르고 "어디서"만 안다 —
    //   한 화면에 두 명이 있으면 영상만으로는 구분할 방법이 없다(person re-ID 필요).
    //   그래서 여럿이면 가장 낮은 id 를 택하되 경고를 찍는다. 조용히 남의 기록으로
    //   붙는 게 제일 나쁘다. 정말 한 카메라에 여러 명을 둬야 한다면 침대별로 ROI 를
    //   나눠 입소자에 매핑하는 작업이 선행돼야 한다.
    //   (이 전제는 여기만의 것이 아니다 — Qt 의 patients[4], care_logs.camera_id,
    //    BedEgressModule 의 채널별 위험도가 전부 같은 가정 위에 서 있다.)
    int getResidentByCamera(int channel);

    // 웨어러블 device_id 로 입소자를 직접 찾는다. 없으면 -1.
    // 웨어러블은 사람마다 다르므로 카메라와 달리 모호함이 전혀 없다.
    // 활동량·생체이상처럼 근거가 웨어러블인 기록은 반드시 이쪽을 쓸 것 —
    // device_id → camera_id → resident 로 돌아가면 같은 카메라에 여러 명이
    // 있을 때 정확했던 정보가 도로 모호해진다.
    int getResidentByWearable(const std::string& wearable_id);

    // 낙상/침상이탈/생체이상 한 건 기록. 반환: event_id, 실패 시 0.
    //  occurredMs : epoch 밀리초. 0 이하면 지금 시각으로 기록한다.
    //  clipUrl    : 블랙박스 클립 주소. 비우면 NULL 로 들어간다.
    // resident_id 는 이 함수가 채널로 찾아 함께 박아둔다 — 조회할 때마다
    // camera_id 로 조인하면, 입소자가 방을 옮긴 뒤 과거 이벤트가 새 입주자
    // 것으로 바뀐다. 발생 시점의 사람을 그대로 남긴다(스냅샷).
    //  residentId : 세 값의 뜻이 다르다 —
    //     > 0 : 호출부가 확정한 사람. 그대로 기록한다.
    //           (웨어러블 → getResidentByWearable, 카메라 → IdentityTracker)
    //       0 : "누구인지 모른다" 를 호출부가 확정한 경우. NULL 로 남긴다.
    //           추적 신뢰도가 낮을 때 쓴다 — 잘못된 이름은 이름 없는 것보다 나쁘다.
    //      -1 : 모르니 cameraId 로 찾아 달라(기본값). 그 방에 여럿이면 첫 사람으로
    //           붙으므로, 사람을 알 수 있는 호출부는 0 이나 실제 id 를 넘길 것.
    long long insertEvent(EventType type, EventSource source, int cameraId,
                          long long occurredMs = 0,
                          const std::string& clipUrl = std::string(),
                          int residentId = -1);

    // 침대 재실 세션 열기(입소자가 침대 ROI 안으로 들어온 순간).
    // 반환: session_id, 실패 시 0. 이 id 를 들고 있다가 나갈 때 닫는다.
    //  residentId : -1 이면 cameraId 로 찾는다(1채널 1인 전제).
    //               ROI 마다 입소자를 매핑한 뒤에는 그 값을 넘길 것 —
    //               한 카메라에 침대가 둘이어도 정확히 갈린다.
    long long openBedSession(int cameraId, int residentId = -1);
    // 재실 세션 닫기(침대에서 벗어난 순간). duration_sec 도 여기서 채운다.
    // 이미 닫힌 세션이면 false — 중복 호출로 시간이 늘어나지 않는다.
    bool closeBedSession(long long sessionId);
    // 부팅 직후 1회. 서버가 재시작되면 메모리에 있던 session_id 가 사라져
    // 열린 행이 영원히 안 닫힌다. 그대로 두면 리포트가 "지금까지 계속 누워있음"
    // 으로 읽어 며칠짜리 재실시간을 만들어낸다. 실제 길이를 알 수 없으니
    // 0 으로 닫는다 — 없는 시간을 지어내느니 덜 세는 편이 낫다.
    // 반환: 정리한 행 수.
    int closeDanglingBedSessions();

    // 웨어러블 1분치 집계 저장(같은 분이면 덮어쓴다).
    // ★ 초 단위 원본 패킷을 그대로 넣지 말 것 — 4명이면 하루 34만 행이 된다.
    //   호출부가 1분치를 메모리에 모았다가 통째로 넘긴다.
    //  minuteEpochSec : 그 분의 시작 시각(epoch 초, 초 단위는 0으로 내림)
    //  stepsDelta     : 그 1분 동안 늘어난 걸음 수 (음수면 호출부가 0으로 보정)
    //  stepsCum       : 웨어러블이 보낸 누적값 원본(리셋/롤오버 추적용)
    //  hrAvg/spo2Avg  : 0 이하면 NULL 로 들어간다(측정 실패)
    bool upsertActivityMinute(int residentId, long long minuteEpochSec,
                              int stepsDelta, int stepsCum,
                              int hrAvg = 0, int spo2Avg = 0);

    void close();
private:
    // 위 공개 함수들이 공통으로 쓰는 "이 카메라의 입소자는 누구인가" 규칙.
    // ★ mutex_ 를 이미 쥔 상태에서 부를 것 — std::mutex 는 재귀 잠금이 안 돼서
    //   getResidentByCamera 를 안에서 다시 부르면 그 자리에서 교착한다.
    //   한 명 초과일 때 경고를 찍는 곳도 여기 하나뿐이다.
    int residentByCameraLocked(int channel);
    // 그 채널의 재원자 "전원". 케어처럼 한 사건이 여러 사람에게 걸리는 기록용.
    // 위 residentByCameraLocked 도 이걸 써서 첫 사람을 고른다(규칙을 한 곳에).
    std::vector<int> residentsByCameraLocked(int channel);

    std::mutex mutex_;  // conn_ 보호
    MYSQL* conn_ = nullptr;
};
#endif
