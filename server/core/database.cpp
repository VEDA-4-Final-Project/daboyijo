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

long long Database::insertCareLog(int cameraId, int durationSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return 0;

    // camera_id·duration은 정수라 인젝션 위험이 없어 snprintf로 안전하게 조립.
    // (문자열 필드를 넣을 땐 반드시 이스케이프 필요 — 나중에 안내)
    // end_time = 지금(NOW()), start_time = 거기서 duration만큼 역산.
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO care_logs (camera_id, duration_sec, start_time, end_time) "
        "VALUES (%d, %d, NOW() - INTERVAL %d SECOND, NOW())",
        cameraId, durationSec, durationSec);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] INSERT 실패: " << mysql_error(conn_) << "\n";
        return 0;
    }
    // 뮤텍스를 쥔 채로 읽어야 한다 — 놓고 읽으면 다른 채널 스레드가 그 사이에
    // INSERT 해서 남의 id를 집어올 수 있다.
    const long long logId = static_cast<long long>(mysql_insert_id(conn_));
    std::cout << "[DB] 케어로그 저장 (카메라 " << (cameraId + 1)
              << ", " << durationSec << "초, log_id=" << logId << ")\n";
    return logId;
}

bool Database::addCareLogDuration(long long logId, int addSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "UPDATE care_logs SET duration_sec = duration_sec + %d, end_time = NOW() "
        "WHERE log_id = %lld",
        addSec, logId);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] 케어로그 병합 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    // 행이 안 맞았으면(누가 지웠다든지) 병합한 척하면 안 된다 — 케어시간이 통째로
    // 사라진다. false를 돌려주면 호출자가 새 행으로 INSERT 하는 쪽으로 넘어간다.
    if (mysql_affected_rows(conn_) == 0) {
        std::cerr << "[DB] 케어로그 병합 대상 없음 (log_id=" << logId << ")\n";
        return false;
    }
    std::cout << "[DB] 케어로그 병합 (log_id=" << logId
              << ", +" << addSec << "초)\n";
    return true;
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

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}
