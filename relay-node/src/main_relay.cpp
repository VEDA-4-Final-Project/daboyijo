// 중계 노드 — HM-10 BLE 수신 → MQTT 발행
// STM32 → HM-10 → BLE → RPi4(이 코드) → 브로커 → master_node
//
// 수신 패킷 7바이트 (firmware/App/Drivers/hm10.h 와 동일 스펙)
//   [0] 0xAA  [1] 심박  [2] SpO2  [3] 낙상 0/1
//   [4] 걸음 lo  [5] 걸음 hi   (uint16, 리틀엔디안)
//   [6] 체크섬 — [0]~[5] 를 XOR
//
// ⚠ 펌웨어와 반드시 동시에 배포할 것. 길이가 어긋나면 프레이밍이 깨진다.

#include "MqttClient_veda.hpp"
#include <simpleble/SimpleBLE.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <unistd.h>
#include <linux/limits.h>

// HM-10 투과모드 표준 UUID — 기기가 바뀌어도 동일
const std::string SVC_FFE0    = "0000ffe0-0000-1000-8000-00805f9b34fb";
const std::string CHAR_FFE1   = "0000ffe1-0000-1000-8000-00805f9b34fb";

const uint8_t PKT_HEADER = 0xAA;
const size_t  PKT_LEN    = 7;
const int QOS_VITAL      = 0;   // 바이탈: 빠르게 (유실 감수)
const int QOS_FALL       = 1;   // 낙상: 반드시 전달

// 기기마다 달라지는 값 — relay-node.conf 에서 읽음
struct Config {
    std::string broker_host  = "localhost";
    int         broker_port  = 1883;
    std::string device_id    = "wearable_01";
    std::string topic        = "veda/wearable/data";
    std::string hm10_addr    = "88:4a:ea:62:0b:03";   // 소문자 MAC
    int         scan_ms      = 8000;
    int         reconnect_ms = 3000;
    bool        debug_hex    = false;                 // 원시 바이트 덤프
    std::string ca_path      = "";                    // 브로커 검증용 CA — 비면 평문 폴백
};

static Config g_cfg;
static std::atomic<bool> g_running{true};
static uint8_t g_prev_fall = 0;   // 낙상 상승엣지 판정용 — 세션 시작 때 초기화, 이후 BLE 콜백 전용

// 조각난 notify 재조립 버퍼 — 전역인 이유는 수명
// unsubscribe 가 실패하면 콜백이 남는데, 지역변수였다면 죽은 스택을 건드림
static std::string g_rx_buffer;

void onSigint(int) { g_running = false; }

std::string toLower(std::string s) {
    // tolower 는 int 를 돌려주지만 입력이 unsigned char 범위면 결과도 그 범위
    for(char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 실행 파일이 있는 디렉터리 — systemd 는 작업 디렉터리가 / 라 상대 경로가 깨짐
std::string exeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

// 오타 하나로 죽지 않게 — 기본값 유지하고 어느 키가 문제인지만 알림
int toInt(const std::string& k, const std::string& v, int fallback) {
    try {
        return std::stoi(v);
    } catch(const std::exception&) {
        std::cerr << "[설정] " << k << " 값이 숫자가 아님: \"" << v
                  << "\" - 기본값 " << fallback << " 사용" << std::endl;
        return fallback;
    }
}

// aa:bb:cc:dd:ee:ff 꼴 검사 — 오타면 "not found" 만 반복돼 기기 문제와 구분이 안 됨
bool looksLikeMac(const std::string& s) {
    if(s.size() != 17) return false;
    for(size_t i = 0; i < s.size(); i++) {
        bool sep = (i % 3 == 2);
        if(sep && s[i] != ':') return false;
        if(!sep && !std::isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// "키 = 값" 형식, # 는 주석 — 없는 키는 기본값 유지
void loadConfig(const std::string& path, Config& c) {
    std::ifstream f(path);
    if(!f) {
        std::cout << "[Relay Node] " << path << " 없음 - 기본값으로 진행" << std::endl;
        return;
    }
    std::string line;
    while(std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        auto eq = line.find('=');
        if(eq == std::string::npos) continue;

        auto trim = [](std::string s) {
            size_t b = s.find_first_not_of(" \t\r");
            size_t e = s.find_last_not_of(" \t\r");
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));

        if     (k == "broker_host")  c.broker_host  = v;
        else if(k == "broker_port")  c.broker_port  = toInt(k, v, c.broker_port);
        else if(k == "device_id")    c.device_id    = v;
        else if(k == "topic")        c.topic        = v;
        else if(k == "scan_ms")      c.scan_ms      = toInt(k, v, c.scan_ms);
        else if(k == "reconnect_ms") c.reconnect_ms = toInt(k, v, c.reconnect_ms);
        else if(k == "debug_hex")    c.debug_hex    = (v != "0");
        else if(k == "ca_path")      c.ca_path      = v;
        else if(k == "hm10_addr") {
            if(looksLikeMac(toLower(v))) c.hm10_addr = toLower(v);
            else std::cerr << "[설정] hm10_addr 이 MAC 형식이 아님: \"" << v
                           << "\" - 기본값 " << c.hm10_addr << " 사용" << std::endl;
        }
    }
}

std::string toHex(const SimpleBLE::ByteArray& bytes) {
    static const char* h = "0123456789abcdef";
    std::string out;
    for(unsigned char c : bytes) { out += h[c >> 4]; out += h[c & 0xF]; out += ' '; }
    return out;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 프레이밍 검증 — 마지막 바이트가 앞 6바이트의 XOR 과 일치해야 함 (★ 펌웨어와 동일)
// 범위 검사는 보조 — 오정렬이 우연히 체크섬을 통과할 때(약 1/256)를 한 번 더 거름
// 심박엔 상한 없음 — 250 으로 자르면 251~255 구간 패킷이 낙상 플래그째 폐기됨
bool packetIsValid(const std::string& buf) {
    uint8_t sum = 0;
    for(size_t i = 0; i + 1 < PKT_LEN; i++) sum ^= (uint8_t)buf[i];
    if(sum != (uint8_t)buf[PKT_LEN - 1]) return false;

    return (uint8_t)buf[2] <= 100 && (uint8_t)buf[3] <= 1;
}

// 패킷 1개 → WearableData → JSON 발행
void publishPacket(uint8_t hr, uint8_t spo2, uint8_t fall, uint16_t steps, MqttClient_veda& client) {
    // 펌웨어가 낙상 플래그를 5초 유지(HM10_FALL_HOLD_MS) — 그대로 흘리면 알람이 5~6번
    // 상승엣지만 낙상으로 침
    bool rising_edge = (fall == 1 && g_prev_fall == 0);
    g_prev_fall = fall;

    WearableData data;
    data.device_id        = g_cfg.device_id;
    data.heart_rate       = hr;
    data.spo2             = spo2;
    data.steps            = steps;
    data.is_fall_detected = rising_edge;
    data.timestamp        = nowMs();   // 패킷에 시각 필드가 없어 수신 시점으로 대체

    nlohmann::json j = data;
    std::string payload = j.dump();

    client.publishMessage(g_cfg.topic, payload, rising_edge ? QOS_FALL : QOS_VITAL);
    std::cout << "[Relay Node] Published " << (rising_edge ? "FALL : " : "vital: ") << payload
              << " (steps=" << steps << ")" << std::endl;
}

// 조각난 데이터를 0xAA 헤더 기준 PKT_LEN 바이트로 재조립
void consumeBuffer(std::string& buf, MqttClient_veda& client) {
    while(true) {
        size_t h = buf.find((char)PKT_HEADER);
        if(h == std::string::npos) { buf.clear(); return; }   // 헤더 없음
        if(h > 0) buf.erase(0, h);                            // 앞쪽 쓰레기 버림
        if(buf.size() < PKT_LEN) return;                      // 덜 모임 — 다음 notify 대기

        if(!packetIsValid(buf)) {
            buf.erase(0, 1);   // 가짜 헤더 → 1바이트 밀고 재동기
            continue;
        }

        // 걸음 수는 2바이트 리틀엔디안 (lo 먼저)
        // | 연산에서 int 로 승격되므로 결과를 다시 uint16_t 로 좁힘 (8+8비트라 손실 없음)
        uint16_t steps = static_cast<uint16_t>(
            static_cast<uint8_t>(buf[4]) | (static_cast<uint8_t>(buf[5]) << 8));
        publishPacket(buf[1], buf[2], buf[3], steps, client);
        buf.erase(0, PKT_LEN);
    }
}

std::optional<SimpleBLE::Peripheral> findDevice(SimpleBLE::Adapter& adapter) {
    std::cout << "[Relay Node] Scanning for HM-10 (max " << g_cfg.scan_ms << "ms)..." << std::endl;
    std::optional<SimpleBLE::Peripheral> found;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        if(!found.has_value() && toLower(p.address()) == g_cfg.hm10_addr) found = p;
    });

    adapter.scan_start();
    auto t0 = std::chrono::steady_clock::now();
    while(!found.has_value() && g_running &&
          std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(g_cfg.scan_ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if(adapter.scan_is_active()) adapter.scan_stop();

    // 위 콜백이 지역변수 found 를 붙잡은 채 어댑터에 남음 — 늦게 불리면 죽은 스택 참조
    adapter.set_callback_on_scan_found([](SimpleBLE::Peripheral){});

    return found;
}

// 연결 1회 세션 — 끊길 때까지 수신 후 정리
void runOnce(SimpleBLE::Adapter& adapter, MqttClient_veda& client) {
    auto dev = findDevice(adapter);
    if(!dev) {
        std::cout << "[Relay Node] HM-10(" << g_cfg.hm10_addr << ") not found. retrying" << std::endl;
        return;
    }

    SimpleBLE::Peripheral peripheral = *dev;
    peripheral.connect();
    std::cout << "[Relay Node] Connected: " << peripheral.identifier() << ". subscribing FFE1" << std::endl;

    g_prev_fall = 0;   // 끊긴 사이 상태를 모르므로 엣지 판정 초기화
    g_rx_buffer.clear();   // 이전 세션의 반쪽 패킷 폐기

    // client 만 캡처 — main 지역변수라 이 함수보다 오래 살고, 버퍼는 전역이라 불필요
    peripheral.notify(SVC_FFE0, CHAR_FFE1, [&client](SimpleBLE::ByteArray bytes) {
        if(g_cfg.debug_hex) {
            std::cout << "[Relay Node] raw " << bytes.size() << "B: " << toHex(bytes) << std::endl;
        }
        g_rx_buffer.append(bytes.begin(), bytes.end());
        consumeBuffer(g_rx_buffer, client);
    });

    while(peripheral.is_connected() && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 정리 안 하면 HM-10 이 연결 상태로 남아 광고를 멈춰 다음 스캔에 안 잡힘
    // 실패해도 로그는 남김 — 콜백이 살아있다는 뜻이라 다음 세션 이상동작의 단서
    try { peripheral.unsubscribe(SVC_FFE0, CHAR_FFE1); }
    catch (const std::exception& e) { std::cerr << "[Relay Node] unsubscribe 실패: " << e.what() << std::endl; }
    catch (...) { std::cerr << "[Relay Node] unsubscribe 실패 (알 수 없는 예외)" << std::endl; }
    try { peripheral.disconnect(); } catch (...) {}
    std::cout << "[Relay Node] Disconnected (cleaned up)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, onSigint);

    std::string confPath = exeDir() + "/relay-node.conf";
    if(argc >= 3 && std::strcmp(argv[1], "-c") == 0) confPath = argv[2];
    loadConfig(confPath, g_cfg);

    // MQTT client id 는 브로커 안에서 유일해야 함 — 겹치면 나중 접속이 먼저 것을 끊음
    // 웨어러블을 여러 대로 늘려도 안 겹치게 device_id 를 붙임
    MqttClient_veda client("relay_" + g_cfg.device_id);

    // ca.crt 도 conf 와 같은 이유로 실행 파일 기준 — systemd 에선 상대 경로가 깨져
    // tls_set 이 실패하고, 평문도 TLS 도 아닌 미연결이 됨
    if(!g_cfg.ca_path.empty() && g_cfg.ca_path[0] != '/')
        g_cfg.ca_path = exeDir() + "/" + g_cfg.ca_path;
    if(!g_cfg.ca_path.empty()) client.setTlsConfig(g_cfg.ca_path);

    std::cout << "[Relay Node] Connecting to Mqtt broker "
              << g_cfg.broker_host << ":" << g_cfg.broker_port
              << (g_cfg.ca_path.empty() ? " (평문)" : " (TLS)") << "..." << std::endl;
    if(!client.connectToBroker(g_cfg.broker_host, g_cfg.broker_port)) {
        // TLS 실패는 대개 인증서 문제라 저절로 안 나음
        // tls_set 이 실패하면 mosquitto_connect 가 호출조차 안 돼 자동 재연결도 안 돎
        // 그대로 두면 BLE 만 돌고 패킷은 큐에 쌓이는 "조용히 죽은" 상태
        if(!g_cfg.ca_path.empty()) {
            std::cerr << "[Relay Node] TLS 연결 실패 — ca_path 확인: " << g_cfg.ca_path
                      << " (종료, systemd 가 재시작한다)" << std::endl;
            return 1;
        }
        // 평문은 버팀 — 브로커가 늦게 떠도 자동 재연결로 붙고 그때 큐가 flush 됨
        std::cerr << "[Relay Node] Mqtt connection failed! buffering until reconnect" << std::endl;
    }
    client.startLoop();

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if(adapters.empty()) {
        std::cerr << "[Relay Node] No BLE adapter found!" << std::endl;
        return 1;
    }
    SimpleBLE::Adapter adapter = adapters.front();
    std::cout << "[Relay Node] Setup complete. adapter: " << adapter.identifier() << std::endl;

    while(g_running) {
        try {
            runOnce(adapter, client);
        } catch (const std::exception& e) {
            std::cerr << "[Relay Node] " << e.what() << std::endl;
        }
        if(g_running) std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.reconnect_ms));
    }

    client.stopLoop();
    return 0;
}
