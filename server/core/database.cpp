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
