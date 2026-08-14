#ifndef DATABASE_HPP
#define DATABASE_HPP
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <mysql.h>   // libmariadb-dev 제공

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
    // 반환: 새로 만들어진 log_id, 실패 시 0. 이 id 를 들고 있어야 요양사가 잠깐
    // 자리를 비웠다 돌아왔을 때 아래 addCareLogDuration 으로 같은 행에 이어붙인다.
    long long insertCareLog(int cameraId, int durationSec);
    // 기존 케어로그에 케어시간을 더한다(자리 비움 후 복귀 → 세션 병합).
    // duration_sec 는 SQL 안에서 더한다 — 읽어서 계산해 쓰면 그 사이에 끼어들 수 있다.
    // start_time 은 그대로 두고 end_time 만 지금으로 민다. 그래서 이 행은
    // end_time - start_time > duration_sec 가 된다(그 차이가 자리 비운 시간).
    bool addCareLogDuration(long long logId, int addSec);
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
    void close();
private:
    std::mutex mutex_;  // conn_ 보호
    MYSQL* conn_ = nullptr;
};
#endif
