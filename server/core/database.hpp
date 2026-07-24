#ifndef DATABASE_HPP
#define DATABASE_HPP
#include <mutex>
#include <string>
#include <mysql.h>   // libmariadb-dev 제공

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
    bool insertCareLog(int cameraId, int durationSec);
    int getPatientStatus(int channel);
    bool updatePatientStatus(int channel, int status);
    void close();
private:
    std::mutex mutex_;  // conn_ 보호
    MYSQL* conn_ = nullptr;
};
#endif
