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
