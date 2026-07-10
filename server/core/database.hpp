#ifndef DATABASE_HPP
#define DATABASE_HPP
#include <string>
#include <mysql.h>   // libmariadb-dev 제공

// DB로 통하는 유일한 문. DB 접속과 SQL은 전부 여기로만.
class Database {
public:
    Database();
    ~Database();
    bool connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& dbname,
                 unsigned int port = 3306);
    // 케어 세션 한 건 기록: 카메라 채널 + 케어시간(초)
    bool insertCareLog(int cameraId, int durationSec);
    void close();
private:
    MYSQL* conn_ = nullptr;
};
#endif
