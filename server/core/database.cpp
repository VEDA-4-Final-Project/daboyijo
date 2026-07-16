#include "database.hpp"
#include <iostream>
#include <cstdio>
#include <cstring>

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

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}
