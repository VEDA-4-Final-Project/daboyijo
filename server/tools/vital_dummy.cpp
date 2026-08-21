// 생체 데이터 더미 발행기 — 웨어러블 없이 SpO2·심박을 계속 쏘는 개발/시연용 도구
//
// 하는 일: DB(residents)에서 재원 중인 입소자를 읽어 그 사람들의 wearable_id 로
// veda/wearable/data 에 WearableData JSON 을 주기적으로(기본 2초) 발행한다.
// 릴레이 노드가 보내는 것과 똑같은 토픽·똑같은 JSON 이라 서버·관제 앱은 진짜
// 웨어러블과 구분하지 못한다 — 코드를 한 줄도 안 고치고 바이탈 화면이 살아난다.
//
// ★ 값은 "정상 범주" 안에서만 움직인다. 서버 알람(HR>180, SpO2<90)은 물론 관제 앱
//   주의 등급(HR>=100 또는 <55, SpO2<95)에도 안 걸리게 여유를 두고 잡았다.
//   알람을 보고 싶으면 이 도구가 아니라 관제 앱의 테스트 버튼을 쓸 것.
//
// ★ 매 틱 난수를 새로 뽑지 않고 직전 값에서 조금씩 움직인다(랜덤 워크).
//   그냥 랜덤이면 2초마다 62→88→65 로 튀어서 추세 그래프가 톱니가 되고
//   한눈에 가짜인 게 보인다. 여기선 사람마다 안정 심박을 따로 두고,
//   가끔 "걷는 구간"이 들어와 심박과 걸음이 같이 오르내린다.
//
// 빌드:  cd server/tools && make
// 실행:  ./vital_dummy --db-host 172.20.32.51 --broker 172.20.32.34 --port 1883
//        TLS 브로커면:  --port 8883 --ca /path/to/ca.crt
#include <mysql.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "MqttClient_veda.hpp"
#include "veda_messages.hpp"

namespace {

std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Options {
    std::string db_host = "127.0.0.1";
    std::string db_user = "daboijo";
    std::string db_pass = "1234";
    std::string db_name = "daboijo";
    std::string broker  = "127.0.0.1";
    int         port    = 1883;
    std::string ca_path;                       // 비우면 평문
    std::string topic   = "veda/wearable/data";
    int         interval_ms = 2000;
    int         limit   = 5;                   // 몇 명분을 쏠지
    bool        all_status = false;            // 재원 필터 없이 전부
    bool        assign_ids = false;            // wearable_id 가 비면 DB 에 채워 넣기
    std::string conf    = "../config/cameras.conf";   // db_host 를 여기서 주워옴
};

void usage() {
    std::cout <<
        "사용법: vital_dummy [옵션]\n"
        "  --db-host H      DB 호스트 (기본: cameras.conf 의 db_host, 없으면 127.0.0.1)\n"
        "  --db-user U      DB 계정 (기본 daboijo)\n"
        "  --db-pass P      DB 비밀번호 (기본 1234)\n"
        "  --db-name N      DB 이름 (기본 daboijo)\n"
        "  --conf PATH      db_host 를 읽어올 설정 파일 (기본 ../config/cameras.conf)\n"
        "  --broker H       MQTT 브로커 호스트 (기본 127.0.0.1)\n"
        "  --port P         브로커 포트 (기본 1883, TLS 면 8883)\n"
        "  --ca PATH        CA 인증서 — 주면 TLS(MQTTS)로 붙는다\n"
        "  --topic T        발행 토픽 (기본 veda/wearable/data)\n"
        "  --interval-ms N  발행 주기 (기본 2000)\n"
        "  --limit N        대상 인원 수 (기본 5)\n"
        "  --all            status='재원' 필터 없이 명단 전체에서 고른다\n"
        "  --assign-ids     wearable_id 가 비어 있으면 wearable_NN 을 DB 에 써 넣는다\n"
        "                   (이 옵션 없이는 DB 를 읽기만 하고, 임시 id 로 발행한다)\n";
}

// key = value 형태 설정 파일에서 한 키만 뽑아온다 (# 뒤는 주석)
std::string readConfValue(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return std::string{};
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return {};
}

// ── 한 사람분의 생체 상태 ─────────────────────────────────────
// 사람마다 안정 심박·SpO2 기준선이 다르다. 다섯 명이 똑같은 숫자를 내면
// 카드가 나란히 붙어 있는 화면에서 바로 티가 난다.
struct Person {
    int         resident_id = 0;
    std::string name;
    std::string device_id;

    double hr_base   = 70.0;   // 안정 심박
    double spo2_base = 97.5;
    double hr        = 70.0;   // 현재 값
    double spo2      = 97.5;
    int    steps     = 0;      // 누적 걸음 (릴레이가 보내는 값과 같은 의미)
    int    walk_ticks_left = 0;
    bool   synthetic_id = false;   // DB 에 wearable_id 가 없어 이 도구가 지어낸 id

    void tick(std::mt19937& rng) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::normal_distribution<double>       n(0.0, 1.0);

        // 걷기 구간: 평소엔 앉아/누워 있고 가끔 30~70초쯤 움직인다
        if (walk_ticks_left > 0) {
            --walk_ticks_left;
        } else if (u(rng) < 0.04) {                      // 2초마다 4% → 대략 1분에 한 번꼴
            walk_ticks_left = 15 + int(u(rng) * 20);     // 30~70초
        }
        const bool walking = walk_ticks_left > 0;

        // 목표 심박으로 서서히 따라간다(1차 지연) + 작은 흔들림
        const double target = hr_base + (walking ? 16.0 : 0.0);
        hr += (target - hr) * 0.25 + n(rng) * 0.8;
        if (hr < 58.0) hr = 58.0;
        if (hr > 95.0) hr = 95.0;                        // 관제 앱 '주의'(>=100) 아래로

        // SpO2 는 원래 거의 안 움직인다 — 96~99 사이에서만 미세하게
        spo2 += n(rng) * 0.25 + (spo2_base - spo2) * 0.2;
        if (spo2 < 96.0) spo2 = 96.0;
        if (spo2 > 99.0) spo2 = 99.0;

        if (walking)               steps += 8 + int(u(rng) * 7);   // 2초에 8~14보
        else if (u(rng) < 0.15)    steps += 1 + int(u(rng) * 3);   // 자리에서 뒤척임
        if (steps > 65535) steps = 0;                              // 릴레이와 같은 2바이트 한계
    }
};

// DB 에서 대상 명단을 읽는다. 못 읽으면 빈 벡터.
std::vector<Person> loadResidents(MYSQL* conn, const Options& opt) {
    std::vector<Person> out;

    std::ostringstream sql;
    sql << "SELECT resident_id, name, IFNULL(wearable_id,'') FROM residents";
    if (!opt.all_status) sql << " WHERE status='재원'";
    sql << " ORDER BY resident_id LIMIT " << opt.limit;

    if (mysql_query(conn, sql.str().c_str())) {
        std::cerr << "[vital_dummy] 입소자 조회 실패: " << mysql_error(conn) << "\n";
        return out;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return out;

    int idx = 0;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        Person p;
        p.resident_id = row[0] ? std::atoi(row[0]) : 0;
        p.name        = row[1] ? row[1] : "";
        p.device_id   = row[2] ? row[2] : "";
        if (p.device_id.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "wearable_%02d", idx + 1);
            p.device_id = buf;
            p.synthetic_id = true;
        }
        ++idx;
        out.push_back(std::move(p));
    }
    mysql_free_result(res);
    return out;
}

// wearable_id 가 비어 있던 사람에게 임시 id 를 DB 에 박아준다(--assign-ids).
// 서버·관제 앱은 device_id → residents.wearable_id 로 사람을 찾으므로, 이게 비면
// 데이터는 흘러도 누구 것인지 몰라 화면에 안 붙는다.
void assignIds(MYSQL* conn, const std::vector<Person>& people) {
    for (const auto& p : people) {
        std::ostringstream sql;
        sql << "UPDATE residents SET wearable_id='" << p.device_id
            << "' WHERE resident_id=" << p.resident_id
            << " AND (wearable_id IS NULL OR wearable_id='')";
        if (mysql_query(conn, sql.str().c_str()))
            std::cerr << "[vital_dummy] wearable_id 지정 실패(" << p.name << "): "
                      << mysql_error(conn) << "\n";
        else if (mysql_affected_rows(conn) > 0)
            std::cout << "[vital_dummy] " << p.name << " → wearable_id=" << p.device_id
                      << " 로 지정했다\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << what << " 값이 없다\n"; std::exit(1); }
            return argv[++i];
        };
        if      (a == "--db-host")    opt.db_host = next("--db-host");
        else if (a == "--db-user")    opt.db_user = next("--db-user");
        else if (a == "--db-pass")    opt.db_pass = next("--db-pass");
        else if (a == "--db-name")    opt.db_name = next("--db-name");
        else if (a == "--conf")       opt.conf    = next("--conf");
        else if (a == "--broker")     opt.broker  = next("--broker");
        else if (a == "--port")       opt.port    = std::atoi(next("--port").c_str());
        else if (a == "--ca")         opt.ca_path = next("--ca");
        else if (a == "--topic")      opt.topic   = next("--topic");
        else if (a == "--interval-ms")opt.interval_ms = std::atoi(next("--interval-ms").c_str());
        else if (a == "--limit")      opt.limit   = std::atoi(next("--limit").c_str());
        else if (a == "--all")        opt.all_status = true;
        else if (a == "--assign-ids") opt.assign_ids = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::cerr << "모르는 옵션: " << a << "\n"; usage(); return 1; }
    }
    if (opt.interval_ms < 100) opt.interval_ms = 100;
    if (opt.limit < 1) opt.limit = 1;

    // --db-host 를 안 줬으면 서버가 쓰는 설정에서 주워온다. 서버와 다른 DB 를 보면
    // 명단이 달라 엉뚱한 사람에게 데이터를 쏘게 된다.
    bool db_host_from_cli = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--db-host") == 0) db_host_from_cli = true;
    if (!db_host_from_cli) {
        const std::string v = readConfValue(opt.conf, "db_host");
        if (!v.empty()) {
            opt.db_host = v;
            std::cout << "[vital_dummy] db_host=" << v << " (" << opt.conf << " 에서 읽음)\n";
        }
    }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // ── DB: 누구에게 쏠지 명단을 읽는다 ────────────────────────
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) { std::cerr << "[vital_dummy] mysql_init 실패\n"; return 1; }
    if (!mysql_real_connect(conn, opt.db_host.c_str(), opt.db_user.c_str(),
                            opt.db_pass.c_str(), opt.db_name.c_str(), 0, nullptr, 0)) {
        std::cerr << "[vital_dummy] DB 연결 실패(" << opt.db_host << "): "
                  << mysql_error(conn) << "\n";
        mysql_close(conn);
        return 1;
    }
    mysql_set_character_set(conn, "utf8mb4");

    std::vector<Person> people = loadResidents(conn, opt);
    if (people.empty()) {
        std::cerr << "[vital_dummy] 대상 입소자가 없다. --all 로 상태 필터를 빼보라\n";
        mysql_close(conn);
        return 1;
    }
    if (opt.assign_ids) assignIds(conn, people);
    mysql_close(conn);   // 명단만 필요하다 — 이후엔 MQTT 만 쓴다

    // 사람마다 기준선을 흩어 놓는다(고정 시드가 아니라 매 실행 다르게)
    std::random_device rd;
    std::mt19937 rng(rd());
    {
        std::uniform_real_distribution<double> hrb(62.0, 78.0);
        std::uniform_real_distribution<double> spb(96.8, 98.4);
        for (auto& p : people) {
            p.hr_base = hrb(rng);   p.hr   = p.hr_base;
            p.spo2_base = spb(rng); p.spo2 = p.spo2_base;
        }
    }

    std::cout << "[vital_dummy] 대상 " << people.size() << "명:\n";
    int synthetic = 0;
    for (const auto& p : people) {
        std::cout << "  - " << p.name << " (id " << p.resident_id
                  << ", device_id " << p.device_id
                  << (p.synthetic_id ? ", DB 에 wearable_id 없음)" : ")") << "\n";
        if (p.synthetic_id) ++synthetic;
    }
    // 이게 제일 흔한 함정이다 — 데이터는 브로커로 잘 나가는데 아무 화면에도 안 뜬다.
    // 서버/관제 앱이 device_id 를 residents.wearable_id 로 사람에게 붙이기 때문.
    if (synthetic && !opt.assign_ids)
        std::cout << "[vital_dummy] ★ 위 " << synthetic << "명은 DB 에 wearable_id 가 비어 있다.\n"
                     "             임시 id 로 발행은 되지만 서버가 누구 것인지 몰라 화면에 안 붙는다.\n"
                     "             --assign-ids 로 다시 실행하면 DB 에 채워 넣는다.\n";

    // ── MQTT ─────────────────────────────────────────────────
    MqttClient_veda client("vital_dummy");
    if (!opt.ca_path.empty()) client.setTlsConfig(opt.ca_path);
    std::cout << "[vital_dummy] 브로커 접속 " << opt.broker << ":" << opt.port
              << (opt.ca_path.empty() ? " (평문)" : " (TLS)") << " ...\n";
    if (!client.connectToBroker(opt.broker, opt.port)) {
        // 평문은 버틴다 — 자동 재연결로 붙고 그때 큐가 나간다. TLS 실패는 대개
        // 인증서 문제라 저절로 안 나으므로 바로 끝낸다.
        if (!opt.ca_path.empty()) {
            std::cerr << "[vital_dummy] TLS 연결 실패 — ca 경로 확인: " << opt.ca_path << "\n";
            return 1;
        }
        std::cerr << "[vital_dummy] 접속 실패 — 재연결될 때까지 큐에 쌓는다\n";
    }
    client.startLoop();

    std::cout << "[vital_dummy] " << opt.interval_ms << "ms 마다 '" << opt.topic
              << "' 로 발행한다. Ctrl+C 로 종료.\n";

    long long ticks = 0;
    while (g_running) {
        const long long ts = nowMs();
        std::ostringstream line;
        for (auto& p : people) {
            p.tick(rng);

            WearableData d;
            d.device_id        = p.device_id;
            d.is_fall_detected = false;      // 낙상은 이 도구가 만들지 않는다
            d.heart_rate       = int(std::lround(p.hr));
            d.spo2             = int(std::lround(p.spo2));
            d.steps            = p.steps;
            d.timestamp        = ts;

            nlohmann::json j = d;
            client.publishMessage(opt.topic, j.dump(), 0);

            line << "  " << p.device_id << " HR=" << d.heart_rate
                 << " SpO2=" << d.spo2 << " steps=" << d.steps;
        }
        // 매 틱 5줄씩 토해내면 로그가 못 볼 물건이 된다 — 한 줄로 묶고,
        // 10틱(=20초)마다 한 번만 찍는다.
        if (ticks % 10 == 0) std::cout << "[vital_dummy]" << line.str() << std::endl;
        ++ticks;

        // 종료 신호에 2초를 다 기다리지 않도록 잘게 쪼개 잔다
        for (int slept = 0; slept < opt.interval_ms && g_running; slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[vital_dummy] 종료한다\n";
    client.stopLoop();
    client.disconnect();
    return 0;
}
