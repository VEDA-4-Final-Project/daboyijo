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

bool Database::insertCareLog(int cameraId, int durationSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;

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
        return false;
    }
    std::cout << "[DB] 케어로그 저장 (카메라 " << cameraId
              << ", " << durationSec << "초)\n";
    return true;
}

// ✨ [추가 구현] 환자 상태 조회 (SELECT)
int Database::getPatientStatus(int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return -1;

    char sql[128];
    std::snprintf(sql, sizeof(sql), 
                  "SELECT status FROM patient_status WHERE camera_id = %d", channel);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] SELECT 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    // 결과 레코드셋 동적 할당 수신
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        std::cerr << "[DB] 결과셋 반환 실패: " << mysql_error(conn_) << "\n";
        return -1;
    }

    int status = -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    
    // 데이터가 존재한다면 정수형 변환 수행
    if (row && row[0]) {
        status = std::atoi(row[0]);
    }

    // ⚠️ 할당받은 결과셋 자원은 반드시 free 해주어야 메모리 누수가 없습니다.
    mysql_free_result(res);
    return status;
}

// ✨ [추가 구현] 부팅 시 위험도 복원 — 진짜 소스인 residents.risk_level에서 읽는다.
// patient_status 테이블은 아무도 INSERT하지 않아 항상 비어 있으므로(→ 전부 '상'으로
// 초기화되는 버그), Qt가 실제로 값을 쓰는 residents를 직접 조회한다.
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

// ✨ [추가 구현] 환자 상태 실시간 동기화 (UPDATE)
bool Database::updatePatientStatus(int channel, int status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_) return false;

    char sql[128];
    std::snprintf(sql, sizeof(sql),
                  "UPDATE patient_status SET status = %d WHERE camera_id = %d", 
                  status, channel);

    if (mysql_query(conn_, sql)) {
        std::cerr << "[DB] UPDATE 실패: " << mysql_error(conn_) << "\n";
        return false;
    }
    return true;
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}
