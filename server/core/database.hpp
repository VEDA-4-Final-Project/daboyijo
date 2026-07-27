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
    // 환자 위험도의 유일한 소스는 residents.risk_level(Qt가 직접 기록)이다.
    // 과거 patient_status 테이블 경로(get/updatePatientStatus)는 아무도 INSERT하지
    // 않아 항상 비어 있던 죽은 경로라 제거함 — 아래 getRiskLevelByCamera로 통일.
    // residents.risk_level(상/중/하)에서 카메라 채널별 위험도를 읽는다(부팅 복원용).
    // 한 채널에 재원 입소자가 여럿이면 가장 높은 위험도(가장 안전한 쪽)를 반환.
    // 반환: 3(상)/2(중)/1(하), 재원 입소자가 없거나 실패 시 -1.
    int getRiskLevelByCamera(int channel);
    void close();
private:
    std::mutex mutex_;  // conn_ 보호
    MYSQL* conn_ = nullptr;
};
#endif
