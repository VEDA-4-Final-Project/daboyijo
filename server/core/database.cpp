#include "database.hpp"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <cstdlib>

Database::Database() {
    conn_ = mysql_init(nullptr);
}

Database::~Database() {
    close();
}

bool Database::connect(const std::string& host, const std::string& user,
                       const std::string& password, const std::string& dbname,
                       unsigned int port) {
    if (!conn_) { std::cerr << "mysql_init 실패\n"; return false; }
    if (!mysql_real_connect(conn_, host.c_str(), user.c_str(),
                            password.c_str(), dbname.c_str(),
                            port, nullptr, 0)) {
        std::cerr << "[DB] 연결 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    mysql_set_character_set(conn_, "utf8mb4");   // 한글 안 깨지게
    std::cout << "[DB] 연결 성공\n";
    return true;
}

std::vector<long long> Database::insertCareLogs(int cameraId, int durationSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<long long> ids;
    if (!conn_) return ids;

    std::vector<int> residents = residentsByCameraLocked(cameraId);
    if (residents.empty()) residents.push_back(-1);

    // 케어 구간을 한 번만 정해 세션 변수에 담는다. NOW() 를 여러 문장에서 각각
    // 부르면 사람마다 몇 밀리초씩 어긋난 창으로 계산돼 같은 방인데 값이 달라진다.
    char win[128];
    std::snprintf(win, sizeof(win),
        "SET @cs = NOW() - INTERVAL %d SECOND, @ce = NOW()", durationSec);
    if (mysql_query(conn_, win)) {
        std::cerr << "[DB] 케어 구간 설정 실패: " << mysql_error(conn_) << "\n";
        return ids;
    }

    // ── 침대 추적이 이 채널에서 실제로 돌고 있는가 ────────────────
    // ★ 안전장치. 재실 기록이 없는데 교집합만 믿으면 모든 케어시간이 0 이 된다.
    //   침대 ROI 를 아직 안 그렸거나 추적이 멈춘 상태가 그렇다. 그때는 "자리에
    //   없었다"가 아니라 "모른다"이므로, 예전처럼 전원에게 전체 시간을 남긴다.
    //   기록이 부풀려지는 것보다 통째로 사라지는 쪽이 더 나쁘다.
    bool bedTracked = false;
    {
        char sql[384];
        std::snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM bed_sessions "
            "WHERE camera_id = %d AND in_at < @ce AND COALESCE(out_at, @ce) > @cs",
            cameraId);
        if (!mysql_query(conn_, sql)) {
            if (MYSQL_RES* res = mysql_store_result(conn_)) {
                if (MYSQL_ROW row = mysql_fetch_row(res))
                    bedTracked = (row[0] && std::atoi(row[0]) > 0);
                mysql_free_result(res);
            }
        }
    }

    for (int residentId : residents) {
        char resident[16];
        if (residentId > 0) std::snprintf(resident, sizeof(resident), "%d", residentId);
        else                std::snprintf(resident, sizeof(resident), "NULL");

        // ── 이 사람에게 실제로 기록할 시간 ────────────────────────
        // 요양사가 방에 있던 구간과 이 사람이 침대에 있던 구간의 교집합.
        // 요양사가 둘 중 누구를 돌봤는지는 여전히 알 수 없지만, 그 시간에 아예
        // 자리에 없던 사람에게 케어시간이 붙는 것은 막을 수 있다.
        //
        // ※ 한계: "침대에 있었나"를 보는 것이지 "방에 있었나"가 아니다. 의자에
        //   앉아 케어받으면 ROI 밖이라 덜 세어진다. 침대 ROI 를 협탁·보조의자까지
        //   넉넉히 그리면 이 오차가 줄어든다(코드 변경 없이 ROI 만 크게).
        char durExpr[512];
        if (bedTracked && residentId > 0) {
            std::snprintf(durExpr, sizeof(durExpr),
                "(SELECT COALESCE(SUM(TIMESTAMPDIFF(SECOND, GREATEST(in_at, @cs), "
                "                                   LEAST(COALESCE(out_at, @ce), @ce))), 0) "
                "   FROM bed_sessions WHERE resident_id = %d "
                "    AND in_at < @ce AND COALESCE(out_at, @ce) > @cs)",
                residentId);
        } else {
            std::snprintf(durExpr, sizeof(durExpr), "%d", durationSec);
        }

        char sql[1024];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO care_logs (camera_id, resident_id, duration_sec, start_time, end_time) "
            "SELECT %d, %s, d, @cs, @ce FROM (SELECT %s AS d) t WHERE d > 0",
            cameraId, resident, durExpr);

        if (mysql_query(conn_, sql)) {
            std::cerr << "[DB] 케어로그 INSERT 실패: " << mysql_error(conn_) << "\n";
            continue;   // 한 사람이 실패해도 나머지는 남긴다
        }
        // WHERE d > 0 이라 겹친 시간이 0 이면 행이 안 만들어진다 — 요양사가 왔을 때
        // 자리에 없던 사람이다. 0초짜리 행을 남기면 "케어 N회" 가 부풀려진다.
        if (mysql_affected_rows(conn_) == 0) {
            std::cout << "[DB] 케어 제외 (카메라 " << (cameraId + 1)
                      << ", 입소자 " << residentId << " — 그 시간 자리에 없었음)\n";
            continue;
        }
        // 뮤텍스를 쥔 채로 읽어야 한다 — 놓고 읽으면 다른 채널 스레드가 그 사이에
        // INSERT 해서 남의 id를 집어올 수 있다.
        ids.push_back(static_cast<long long>(mysql_insert_id(conn_)));
    }

    std::cout << "[DB] 케어로그 저장 (카메라 " << (cameraId + 1) << ", "
              << durationSec << "초, " << ids.size() << "명"
              << (bedTracked ? ", 재실 교집합 적용" : ", 재실 기록 없어 전체 시간") << ")\n";
    return ids;
}

bool Database::addCareLogDuration(const std::vector<long long>& logIds, int addSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || logIds.empty()) return false;

    // 한 행이라도 못 붙이면 false 를 돌려준다. 일부만 합산되면 같은 방 사람끼리
    // 케어시간이 어긋나므로, 호출자가 전원에게 새 행을 만드는 쪽이 낫다.
    bool allOk = true;
    for (long long logId : logIds) {
        char sql[256];
        std::snprintf(sql, sizeof(sql),
            "UPDATE care_logs SET duration_sec = duration_sec + %d, end_time = NOW() "
            "WHERE log_id = %lld",
            addSec, logId);

        if (mysql_query(conn_, sql)) {
            std::cerr << "[DB] 케어로그 병합 실패: " << mysql_error(conn_) << "\n";
            allOk = false;
            continue;
        }
        // 행이 안 맞았으면(누가 지웠다든지) 병합한 척하면 안 된다 — 케어시간이
        // 통째로 사라진다. false 를 돌려주면 호출자가 새 행 INSERT 로 넘어간다.
        if (mysql_affected_rows(conn_) == 0) {
            std::cerr << "[DB] 케어로그 병합 대상 없음 (log_id=" << logId << ")\n";
            allOk = false;
        }
    }
    if (allOk)
        std::cout << "[DB] 케어로그 병합 (" << logIds.size() << "행, +" << addSec << "초)\n";
    return allOk;
}

// 부팅 시 위험도 복원 — 진짜 소스인 residents.risk_level에서 읽는다.
// (과거 patient_status 테이블은 아무도 INSERT하지 않아 항상 비어 있던 죽은 경로였다.)
// Qt가 실제로 값을 쓰는 residents를 직접 조회한다.
// 한 채널에 재원 입소자가 여럿이면 MAX로 가장 높은 위험도(가장 안전한 쪽)를 택한다.
int Database::getRiskLevelByCamera(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    // 텍스트(상/중/하) → 정수(3/2/1) 매핑을 SQL에서 처리. 알 수 없는 값은 안전상 3(상).
    // 재원('재원') 입소자만 대상 — 퇴원자가 채널을 물고 있지 않게.
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT MAX(CASE risk_level "
        "WHEN '상' THEN 3 WHEN '중' THEN 2 WHEN '하' THEN 1 ELSE 3 END) "
        "FROM residents WHERE camera_id = %d AND status = '재원'",
        channel);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 위험도 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 위험도 결과셋 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    int level = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    // 집계 쿼리는 항상 한 행을 반환하지만, 대상 입소자가 없으면 그 값이 NULL이다.
    if (row && row[0]) {
        level = std::atoi(row[0]);
    }
    mysql_free_result(res);
    return level;
}

// 침대에 매핑된 입소자 1명의 위험도. 채널 MAX 방식과 달리 그 사람 값만 본다.
int Database::getRiskLevelByResident(int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || residentId <= 0) return -1;

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT CASE risk_level "
        "WHEN '상' THEN 3 WHEN '중' THEN 2 WHEN '하' THEN 1 ELSE 3 END "
        "FROM residents WHERE resident_id = %d", residentId);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 입소자 위험도 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return -1;

    int level = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) level = std::atoi(row[0]);
    mysql_free_result(res);
    return level;
}

std::string Database::getResidentName(int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || residentId <= 0) return std::string();

    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "SELECT name FROM residents WHERE resident_id = %d", residentId);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 입소자 이름 조회 실패: " << mysql_error(conn_) << "\n";
        return std::string();
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return std::string();

    std::string name;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) name = row[0];
    mysql_free_result(res);
    return name;
}

int Database::getRoomByResident(int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || residentId <= 0) return -1;

    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "SELECT room FROM residents WHERE resident_id = %d", residentId);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 입소자 호실 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return -1;

    int room = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) room = std::atoi(row[0]);
    mysql_free_result(res);
    return room;
}

// ── 침대 ROI 영속화 ──────────────────────────────────────────────
// roi_points 는 [[x,y],[x,y],...] 형태의 JSON 배열(정규화 0~1)로 저장한다.
bool Database::saveRoiZone(int cameraId, int roiId,
                           const std::vector<std::pair<float, float>>& points) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;
    if (points.size() < 3 || points.size() > 32) return false;  // 프로토콜 상한과 동일

    // 좌표는 float라 인젝션 위험이 없다 — 문자열이 아니므로 이스케이프 대상도 아니다.
    std::string json = "[";
    char buf[64];
    for (size_t i = 0; i < points.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s[%.4f,%.4f]", i ? "," : "",
                      points[i].first, points[i].second);
        json += buf;
    }
    json += "]";

    // ★ INSERT ... ON DUPLICATE KEY UPDATE — 같은 침대를 다시 그리면 덮어쓴다.
    //   resident_id는 건드리지 않는다(각도만 고쳐 그렸는데 사람이 떨어지면 안 됨).
    std::string sql =
        "INSERT INTO roi_zones (camera_id, roi_id, roi_name, roi_points) VALUES (" +
        std::to_string(cameraId) + ", " + std::to_string(roiId) + ", '', '" + json +
        "') ON DUPLICATE KEY UPDATE roi_points = VALUES(roi_points)";

    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "[DB] 침대 ROI 저장 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    std::cout << "[DB] 침대 ROI 저장 (ch" << (cameraId + 1) << " 침대"
              << (roiId + 1) << ", " << points.size() << "점)\n";
    return true;
}

bool Database::bindRoiZoneResident(int cameraId, int roiId, int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;

    // 아직 ROI를 안 그린 침대에 사람만 먼저 지정할 수도 있어 INSERT ... ON DUPLICATE.
    // 그 경우 roi_points는 빈 배열로 두고, 나중에 saveRoiZone이 채운다.
    char sql[256];
    if (residentId > 0) {
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO roi_zones (camera_id, roi_id, roi_name, roi_points, resident_id) "
            "VALUES (%d, %d, '', '[]', %d) "
            "ON DUPLICATE KEY UPDATE resident_id = VALUES(resident_id)",
            cameraId, roiId, residentId);
    } else {
        std::snprintf(sql, sizeof(sql),
            "UPDATE roi_zones SET resident_id = NULL "
            "WHERE camera_id = %d AND roi_id = %d", cameraId, roiId);
    }

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 침대 입소자 매핑 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    std::cout << "[DB] 침대 입소자 매핑 (ch" << (cameraId + 1) << " 침대"
              << (roiId + 1) << " → 입소자 " << residentId << ")\n";
    return true;
}

bool Database::deleteRoiZone(int cameraId, int roiId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;

    char sql[160];
    if (roiId < 0) {
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM roi_zones WHERE camera_id = %d", cameraId);
    } else {
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM roi_zones WHERE camera_id = %d AND roi_id = %d",
            cameraId, roiId);
    }

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 침대 ROI 삭제 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    return true;
}

std::vector<RoiZoneRow> Database::loadRoiZones() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoiZoneRow> rows;
    if (!conn_) return rows;

    if (mysql_query(conn_,
            "SELECT camera_id, roi_id, resident_id, roi_points FROM roi_zones "
            "ORDER BY camera_id, roi_id")) {
        std::cerr << "[DB] 침대 ROI 로드 실패: " << mysql_error(conn_) << "\n";
        return rows;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return rows;

    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        if (!row[0] || !row[1] || !row[3]) continue;
        RoiZoneRow z;
        z.camera_id = std::atoi(row[0]);
        z.roi_id = std::atoi(row[1]);
        z.resident_id = row[2] ? std::atoi(row[2]) : 0;

        // roi_points는 우리가 쓴 형식([[x,y],...])이라 숫자만 순서대로 긁어
        // 둘씩 묶으면 된다 — 이 컬럼에 다른 구조가 들어올 경로가 없다.
        const char* p = row[3];
        std::vector<float> nums;
        while (*p) {
            if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
                char* end = nullptr;
                nums.push_back(std::strtof(p, &end));
                p = (end && end != p) ? end : p + 1;
            } else {
                ++p;
            }
        }
        for (size_t i = 0; i + 1 < nums.size(); i += 2) {
            z.points.emplace_back(nums[i], nums[i + 1]);
        }
        rows.push_back(std::move(z));
    }
    mysql_free_result(res);
    std::cout << "[DB] 침대 ROI " << rows.size() << "건 로드\n";
    return rows;
}

int Database::getCHById(const std::string& wearable_id){
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    /* device_id 는 MQTT 페이로드로 들어온 외부 입력이다. 정수 파라미터와 달리
     * 문자열을 그대로 SQL 에 넣으면 인젝션이 성립하므로 반드시 이스케이프한다.
     * 버퍼는 원문 길이의 2배 + 1 이 필요하다 (모든 문자가 이스케이프될 때). */
    char esc[65];
    if (wearable_id.size() > 32) return -1;   // 컬럼 정의(VARCHAR(32))를 넘으면 조회할 것도 없다
    mysql_real_escape_string(conn_, esc, wearable_id.c_str(),
                             (unsigned long)wearable_id.size());

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT camera_id "
        "FROM residents WHERE wearable_id = '%s'", esc);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    int ch = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    // 집계 쿼리는 항상 한 행을 반환하지만, 대상 입소자가 없으면 그 값이 NULL이다.
    if (row && row[0]) {
        ch = std::atoi(row[0]);
    }
    mysql_free_result(res);
    return ch;
}

int Database::getRoomById(const std::string& wearable_id){
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    char esc[65];
    if (wearable_id.size() > 32) return -1;
    mysql_real_escape_string(conn_, esc, wearable_id.c_str(),
                             (unsigned long)wearable_id.size());

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT room "
        "FROM residents WHERE wearable_id = '%s'", esc);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    int room = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    // 집계 쿼리는 항상 한 행을 반환하지만, 대상 입소자가 없으면 그 값이 NULL이다.
    if (row && row[0]) {
        room = std::atoi(row[0]);
    }
    mysql_free_result(res);
    return room;
}

int Database::getRoomByCh(int channel){
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT room "
        "FROM residents WHERE camera_id = %d", channel);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    int room = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    // 집계 쿼리는 항상 한 행을 반환하지만, 대상 입소자가 없으면 그 값이 NULL이다.
    if (row && row[0]) {
        room = std::atoi(row[0]);
    }
    mysql_free_result(res);
    return room;
}

// ═══════════════════════════════════════════════════════════
//  일일 리포트용 기록 (report_schema.sql)
// ═══════════════════════════════════════════════════════════

namespace {
// 같은 사건으로 볼 시간 창. 펌웨어가 낙상 플래그를 유지하는 시간과 맞춘다
// (firmware HM10_FALL_HOLD_MS = 5000ms) — 그 창을 넘으면 별개 사건으로 센다.
constexpr int kDedupWindowSec = 5;

// 열거형 → ENUM 컬럼 문자열. report_schema.sql 의 ENUM 정의와 글자가
// 하나라도 어긋나면 INSERT 가 실패한다 — 값을 늘릴 땐 양쪽을 같이 고칠 것.
const char* toSql(EventType t) {
    switch (t) {
        case EventType::Fall:          return "FALL";
        case EventType::BedEgress:     return "EGRESS";
        case EventType::VitalAbnormal: return "VITAL_ABNORMAL";
    }
    return "FALL";   // 도달 불가 — 컴파일러 경고 억제용
}
const char* toSql(EventSource s) {
    switch (s) {
        case EventSource::Wearable: return "WEARABLE";
        case EventSource::Both:     return "BOTH";
        case EventSource::Camera:   break;
    }
    return "CAMERA";
}
}  // namespace

// mutex_ 를 이미 쥔 상태에서만 부를 것 (헤더 주석 참고).
// "이 카메라에 있는 재원자는 누구누구인가" 규칙은 여기 한 곳에만 둔다.
std::vector<int> Database::residentsByCameraLocked(int channel) {
    std::vector<int> ids;
    if (!conn_) return ids;

    // 퇴원자가 채널을 물고 있지 않게 재원자만 본다(getRiskLevelByCamera 와 같은 규칙).
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT resident_id FROM residents "
        "WHERE camera_id = %d AND status = '재원' ORDER BY resident_id",
        channel);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 입소자 조회 실패: " << mysql_error(conn_) << "\n";
        return ids;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 입소자 결과셋 반환 실패: " << mysql_error(conn_) << "\n";
        return ids;
    }
    while (MYSQL_ROW row = mysql_fetch_row(res))
        if (row[0]) ids.push_back(std::atoi(row[0]));
    mysql_free_result(res);
    return ids;
}

int Database::residentByCameraLocked(int channel) {
    const std::vector<int> ids = residentsByCameraLocked(channel);
    if (ids.empty()) return -1;

    // 한 사람만 고를 수 있는 기록(낙상 등)인데 방에 여럿이면 영상만으로는 정할 수
    // 없다. 첫 사람으로 붙이되, 리포트가 틀렸을 때 여기부터 의심하도록 남긴다.
    // (케어시간은 이 경로를 쓰지 않는다 — insertCareLogs 가 전원에게 기록한다)
    if (ids.size() > 1) {
        std::cerr << "[DB] ⚠ 카메라 " << (channel + 1) << " 에 재원자가 " << ids.size()
                  << "명 — 기록을 resident_id=" << ids.front()
                  << " 로 붙인다. 침대 ROI 로 사람을 특정하면 정확해진다.\n";
    }
    return ids.front();
}

int Database::getResidentByCamera(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    return residentByCameraLocked(channel);
}

int Database::getResidentByWearable(const std::string& wearable_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    // device_id 는 MQTT 로 들어온 외부 입력이다 — 반드시 이스케이프(getCHById 와 동일).
    if (wearable_id.size() > 32) return -1;   // VARCHAR(32) 를 넘으면 조회할 것도 없다
    char esc[65];
    mysql_real_escape_string(conn_, esc, wearable_id.c_str(),
                             (unsigned long)wearable_id.size());

    // wearable_id 에는 UNIQUE 제약이 걸려 있어(schema.sql) 여러 명이 나올 수 없다.
    // 카메라 조회와 달리 경고 분기가 필요 없는 이유가 이것이다.
    // status 를 따지지 않는 것도 의도적이다 — 퇴원 처리가 늦어도 웨어러블이
    // 보내는 값은 그 사람 것이 맞다.
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT resident_id FROM residents WHERE wearable_id = '%s'", esc);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 웨어러블 입소자 조회 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 웨어러블 결과셋 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }
    int id = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) id = std::atoi(row[0]);
    mysql_free_result(res);
    return id;
}

long long Database::insertEvent(EventType type, EventSource source, int cameraId,
                                long long occurredMs, const std::string& clipUrl,
                                int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return 0;

    // 호출부가 이미 정확히 아는 경우(웨어러블 → getResidentByWearable)는 그 값을
    // 쓰고, 아니면 카메라로 찾는다. 못 찾으면 NULL — 사람 없이라도 사건은 남긴다.
    if (residentId < 0) residentId = residentByCameraLocked(cameraId);
    char resident[16];
    if (residentId > 0) std::snprintf(resident, sizeof(resident), "%d", residentId);
    else                std::snprintf(resident, sizeof(resident), "NULL");

    // clip_url 은 채널·타임스탬프로 우리가 만든 문자열이지만, 문자열을 SQL 에
    // 넣을 땐 예외 없이 이스케이프한다(길이 = 원문의 2배 + 1).
    if (clipUrl.size() > 255) {   // VARCHAR(255) — 넘으면 잘려 들어가느니 버린다
        std::cerr << "[DB] clip_url 이 너무 김 (" << clipUrl.size() << ") — NULL 로 기록\n";
    }
    char esc[512] = {};
    const bool hasClip = !clipUrl.empty() && clipUrl.size() <= 255;
    if (hasClip)
        mysql_real_escape_string(conn_, esc, clipUrl.c_str(),
                                 (unsigned long)clipUrl.size());

    // occurred_at 은 DATETIME(3) — 밀리초까지 남긴다. Qt 와 서버가 주고받는
    // timestamp 가 epoch ms 라 그대로 넣을 수 있다. 0 이하면 지금 시각.
    char when[64];
    if (occurredMs > 0)
        std::snprintf(when, sizeof(when), "FROM_UNIXTIME(%lld/1000.0)", occurredMs);
    else
        std::snprintf(when, sizeof(when), "NOW(3)");

    // ── 같은 사건이 두 경로로 들어온 경우 합치기 ──────────────────
    // 한 번 넘어져도 카메라(자세 판정)와 웨어러블(가속도)이 각각 잡아 두 줄이 된다.
    // 그대로 두면 리포트가 "낙상 2회"로 센다 — 실제로는 1회다.
    //
    // ★ 기다리지 않는다. 먼저 온 쪽은 즉시 INSERT 하고, 나중에 온 쪽이 앞을 돌아봐
    //   같은 사건이 있으면 그 줄의 source 를 BOTH 로 바꾼다. 대기가 없으니 알람도
    //   늦지 않는다(알람은 이 함수와 무관하게 이미 나간 뒤다).
    // ★ 창은 5초 — 펌웨어가 낙상 플래그를 들고 있는 시간(HM10_FALL_HOLD_MS)과 같다.
    //   그 안에 들어온 같은 사람·같은 종류는 한 사건으로 본다.
    // ★ 같은 source 끼리는 합치지 않는다. 카메라는 fired 플래그로, 웨어러블은 래치로
    //   이미 중복을 막고 있어서, 같은 쪽이 5초 안에 두 번 오면 그건 진짜 두 번이다.
    // ★ BOTH 는 정보가 주는 게 아니라 는다 — 양쪽이 함께 본 낙상이라 더 확실하다.
    {
        // 사람을 모르면(resident_id NULL) 채널로 짝을 찾는다. 사람이 없는 기록끼리
        // 합치는 것은 근거가 약하지만, 같은 채널·같은 종류·5초 안이라면 같은 사건일
        // 가능성이 훨씬 높다 — 두 줄로 남겨 2회로 세는 쪽이 더 틀린다.
        char match[256];
        if (residentId > 0)
            std::snprintf(match, sizeof(match), "resident_id = %d", residentId);
        else
            std::snprintf(match, sizeof(match), "resident_id IS NULL AND camera_id = %d",
                          cameraId);

        char sql[768];
        std::snprintf(sql, sizeof(sql),
            "SELECT event_id FROM events "
            "WHERE %s AND event_type = '%s' AND source <> '%s' "
            "  AND occurred_at BETWEEN %s - INTERVAL %d SECOND "
            "                      AND %s + INTERVAL %d SECOND "
            "ORDER BY event_id DESC LIMIT 1",
            match, toSql(type), toSql(source),
            when, kDedupWindowSec, when, kDedupWindowSec);

        if (!mysql_query(conn_, sql)) {
            if (MYSQL_RES* res = mysql_store_result(conn_)) {
                long long twinId = 0;
                if (MYSQL_ROW row = mysql_fetch_row(res))
                    if (row[0]) twinId = std::atoll(row[0]);
                mysql_free_result(res);

                if (twinId > 0) {
                    char up[128];
                    std::snprintf(up, sizeof(up),
                        "UPDATE events SET source = 'BOTH' WHERE event_id = %lld", twinId);
                    if (mysql_query(conn_, up)) {
                        std::cerr << "[DB] 이벤트 병합 실패: " << mysql_error(conn_) << "\n";
                        // 병합에 실패했으면 새 줄이라도 남긴다 — 아래로 흘려보낸다.
                    } else {
                        std::cout << "[DB] 이벤트 병합 (" << toSql(type)
                                  << ", event_id=" << twinId << " → BOTH)\n";
                        return twinId;   // 새 줄을 만들지 않는다
                    }
                }
            }
        } else {
            std::cerr << "[DB] 이벤트 중복 조회 실패: " << mysql_error(conn_) << "\n";
            // 조회가 안 되면 합치기를 포기하고 그냥 새 줄로 남긴다.
        }
    }

    // resident_id 는 지금 값을 그대로 박는다(발생 시점 스냅샷 — 헤더 주석 참고).
    char sql[1024];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO events "
        "(occurred_at, camera_id, resident_id, event_type, source, clip_url) "
        "VALUES (%s, %d, %s, '%s', '%s', %s%s%s)",
        when, cameraId, resident, toSql(type), toSql(source),
        hasClip ? "'" : "", hasClip ? esc : "NULL", hasClip ? "'" : "");

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 이벤트 기록 실패: " << mysql_error(conn_) << "\n";
        return 0;
    }
    const long long eventId = static_cast<long long>(mysql_insert_id(conn_));
    std::cout << "[DB] 이벤트 기록 (" << toSql(type) << ", 카메라 " << (cameraId + 1)
              << ", event_id=" << eventId << ")\n";
    return eventId;
}

std::vector<EventRow> Database::findEvents(bool anyType, EventType type, int channel,
                                           long long startMs, long long endMs,
                                           int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EventRow> rows;
    if (!conn_) return rows;
    if (limit <= 0) limit = 5;
    if (limit > 50) limit = 50;  // 방어적 상한 — 호출부 실수로도 폭주 안 하게

    // type/channel 은 전부 우리 코드가 만든 enum·int라 이스케이프가 필요한
    // 사용자 원문 문자열이 섞이지 않는다(insertEvent 와 같은 전제).
    char typeClause[48] = "";
    if (!anyType) {
        std::snprintf(typeClause, sizeof(typeClause), "AND event_type = '%s' ", toSql(type));
    }
    char channelClause[32] = "";
    if (channel >= 0) {
        std::snprintf(channelClause, sizeof(channelClause), "AND camera_id = %d ", channel);
    }

    char sql[768];
    std::snprintf(sql, sizeof(sql),
        "SELECT event_id, UNIX_TIMESTAMP(occurred_at)*1000, camera_id, resident_id, "
        "event_type, source, clip_url FROM events "
        "WHERE occurred_at BETWEEN FROM_UNIXTIME(%lld/1000.0) AND FROM_UNIXTIME(%lld/1000.0) "
        "%s%sORDER BY occurred_at DESC LIMIT %d",
        startMs, endMs, typeClause, channelClause, limit);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 이벤트 검색 실패: " << mysql_error(conn_) << "\n";
        return rows;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 이벤트 검색 결과셋 반환 실패: " << mysql_error(conn_) << "\n";
        return rows;
    }
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        EventRow r;
        r.event_id = row[0] ? std::atoll(row[0]) : 0;
        r.occurred_ms = row[1] ? std::atoll(row[1]) : 0;
        r.camera_id = row[2] ? std::atoi(row[2]) : -1;
        r.resident_id = row[3] ? std::atoi(row[3]) : -1;
        const std::string typeStr = row[4] ? row[4] : "";
        r.type = typeStr == "EGRESS"         ? EventType::BedEgress
                 : typeStr == "VITAL_ABNORMAL" ? EventType::VitalAbnormal
                                                : EventType::Fall;
        r.source = (row[5] && std::string(row[5]) == "WEARABLE") ? EventSource::Wearable
                                                                  : EventSource::Camera;
        r.clip_url = row[6] ? row[6] : "";
        rows.push_back(std::move(r));
    }
    mysql_free_result(res);
    return rows;
}

long long Database::openBedSession(int cameraId, int residentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return 0;

    // 호출부가 ROI 매핑으로 이미 정확히 아는 경우엔 그 값을 그대로 쓴다.
    if (residentId < 0) residentId = residentByCameraLocked(cameraId);
    char resident[16];
    if (residentId > 0) std::snprintf(resident, sizeof(resident), "%d", residentId);
    else                std::snprintf(resident, sizeof(resident), "NULL");

    char sql[512];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO bed_sessions (camera_id, resident_id, in_at) "
        "VALUES (%d, %s, NOW())",
        cameraId, resident);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 재실 세션 시작 실패: " << mysql_error(conn_) << "\n";
        return 0;
    }
    const long long sessionId = static_cast<long long>(mysql_insert_id(conn_));
    std::cout << "[DB] 재실 시작 (카메라 " << (cameraId + 1)
              << ", session_id=" << sessionId << ")\n";
    return sessionId;
}

bool Database::closeBedSession(long long sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || sessionId <= 0) return false;

    // out_at IS NULL 조건이 핵심이다. 이게 없으면 같은 세션을 두 번 닫을 때
    // out_at 이 뒤로 밀리면서 재실시간이 계속 늘어난다.
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "UPDATE bed_sessions "
        "SET out_at = NOW(), duration_sec = TIMESTAMPDIFF(SECOND, in_at, NOW()) "
        "WHERE session_id = %lld AND out_at IS NULL",
        sessionId);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 재실 세션 종료 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    if (mysql_affected_rows(conn_) == 0) {
        std::cerr << "[DB] 닫을 재실 세션 없음 (session_id=" << sessionId << ")\n";
        return false;
    }
    std::cout << "[DB] 재실 종료 (session_id=" << sessionId << ")\n";
    return true;
}

int Database::closeDanglingBedSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return 0;

    // 지어낸 시간을 남기지 않으려고 in_at 그대로 닫고 0초로 만든다(헤더 주석 참고).
    if (mysql_query(conn_,
            "UPDATE bed_sessions SET out_at = in_at, duration_sec = 0 "
            "WHERE out_at IS NULL")) {
        std::cerr << "[DB] 미종료 재실 세션 정리 실패: " << mysql_error(conn_) << "\n";
        return 0;
    }
    const int n = static_cast<int>(mysql_affected_rows(conn_));
    if (n > 0)
        std::cout << "[DB] 재시작 전 미종료 재실 세션 " << n << "건 정리\n";
    return n;
}

bool Database::upsertActivityMinute(int residentId, long long minuteEpochSec,
                                    int stepsDelta, int stepsCum,
                                    int hrAvg, int spo2Avg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || residentId < 0) return false;

    // 측정 실패(0)를 0bpm 으로 남기면 평균이 망가진다 — NULL 로 넣는다.
    char hr[16], spo2[16];
    if (hrAvg   > 0) std::snprintf(hr,   sizeof(hr),   "%d", hrAvg);
    else             std::snprintf(hr,   sizeof(hr),   "NULL");
    if (spo2Avg > 0) std::snprintf(spo2, sizeof(spo2), "%d", spo2Avg);
    else             std::snprintf(spo2, sizeof(spo2), "NULL");

    // 같은 분이 다시 오면 통째로 덮어쓴다(더하지 않는다). 호출부가 그 1분치
    // 누적을 들고 있다가 넘기는 구조라, 더하면 이중 계산이 된다.
    char sql[512];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO activity_minute "
        "(resident_id, minute_ts, steps_delta, steps_cum, hr_avg, spo2_avg) "
        "VALUES (%d, FROM_UNIXTIME(%lld), %d, %d, %s, %s) "
        "ON DUPLICATE KEY UPDATE "
        "steps_delta = VALUES(steps_delta), steps_cum = VALUES(steps_cum), "
        "hr_avg = VALUES(hr_avg), spo2_avg = VALUES(spo2_avg)",
        residentId, minuteEpochSec, stepsDelta, stepsCum, hr, spo2);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 활동량 기록 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    return true;
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}
