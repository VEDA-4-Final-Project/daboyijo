// 중계 노드 — HM-10 BLE 수신 → MQTT 발행
// STM32 → HM-10 → BLE → RPi4(이 코드) → 브로커 → master_node
//
// 수신 패킷 7바이트 (firmware/App/Drivers/hm10.h 와 동일 스펙)
//   [0] 0xAA  [1] 심박  [2] SpO2  [3] 낙상 0/1
//   [4] 걸음 lo  [5] 걸음 hi   (uint16, 리틀엔디안)
//   [6] 체크섬 — [0]~[5] 를 XOR
//
// ⚠ 펌웨어와 반드시 동시에 배포 — 길이가 어긋나면 프레이밍이 깨짐

#include "MqttClient_veda.hpp"
#include <simpleble/SimpleBLE.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// ── 시각 동기 (RPi → STM32) ───────────────────────────────────────
//   [0] 0x55  [1] 시  [2] 분  [3] 초  [4] 체크섬([0]~[3] XOR)
//
// 웨어러블에는 RTC 도 백업 배터리도 없어 부팅하면 빌드 시각으로 시작한다.
// NTP 로 맞춰진 이쪽 시각을 내려보내 화면의 시계와 자정 걸음수 리셋을 맞춘다.
// 주기적으로 다시 보내 SysTick 드리프트(하루 약 2초)도 함께 잡는다.
const uint8_t TIME_PKT_HEADER = 0x55;
const size_t  TIME_PKT_LEN    = 5;
const int     TIME_SYNC_MS    = 10 * 60 * 1000;   // 10분마다 (연결 직후에도 1회)

// ── 시각 요청 (STM32 → RPi) ───────────────────────────────────────
//   [0] 0xA5  [1] 0x01(시각)  [2] 체크섬([0]^[1])
//
// 웨어러블이 부팅하면 빌드 시각으로 시작한다. 위의 10분 주기만 기다리면
// 그동안 화면에 틀린 시각이 남는데, MCU 만 리셋되고 BLE 링크는 살아있는 경우
// 이쪽은 아무 일도 없었다고 보기 때문에 특히 오래 걸린다.
// 웨어러블이 먼저 물어보면 그 자리에서 답해준다.
const uint8_t REQ_HEADER   = 0xA5;
const size_t  REQ_LEN      = 3;
const uint8_t REQ_TIME     = 0x01;

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

// 웨어러블이 시각을 요청했다. notify 콜백(BLE 스레드)이 세우고 runOnce 루프가 소비한다.
// 콜백 안에서 곧바로 write 하지 않는 이유는 SimpleBLE 재진입을 피하기 위해서다 —
// 펌웨어에서 ISR 이 플래그만 세우고 메인 루프가 처리하는 것과 같은 이유다.
static std::atomic<bool> g_time_requested{false};

// 첫 신호는 정상 종료 요청, 두 번째는 강제 탈출
// 플래그를 보는 곳이 스캔·연결 유지 루프뿐이라 DBus 안에 있으면 Ctrl+C 가 안 먹음
void onSignal(int) {
    if(!g_running) std::_Exit(1);   // 두 번째 신호 — 정리 포기
    g_running = false;
}

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

// 시각 요청 검증 — 종류 바이트와 체크섬을 모두 본다.
bool requestIsValid(const std::string& buf) {
    return (uint8_t)buf[1] == REQ_TIME &&
           (uint8_t)((uint8_t)buf[0] ^ (uint8_t)buf[1]) == (uint8_t)buf[2];
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

// 조각난 데이터를 헤더 기준으로 재조립한다.
// 웨어러블이 올려보내는 것은 두 종류다 — 바이탈(0xAA, 7B)과 시각 요청(0xA5, 3B).
// 맨 앞 바이트를 보고 갈라내며, 어느 쪽 헤더도 아니면 1바이트씩 버려 재동기한다.
void consumeBuffer(std::string& buf, MqttClient_veda& client) {
    while(!buf.empty()) {
        const uint8_t head = (uint8_t)buf[0];

        if(head == PKT_HEADER) {                  // ── 바이탈 ──
            if(buf.size() < PKT_LEN) return;      // 덜 모임 — 다음 notify 대기
            if(!packetIsValid(buf)) {
                buf.erase(0, 1);                  // 가짜 헤더 → 1바이트 밀고 재동기
                continue;
            }

            // 걸음 수는 2바이트 리틀엔디안 (lo 먼저)
            // | 연산에서 int 로 승격되므로 결과를 다시 uint16_t 로 좁힘 (8+8비트라 손실 없음)
            uint16_t steps = static_cast<uint16_t>(
                static_cast<uint8_t>(buf[4]) | (static_cast<uint8_t>(buf[5]) << 8));
            publishPacket(buf[1], buf[2], buf[3], steps, client);
            buf.erase(0, PKT_LEN);
            continue;
        }

        if(head == REQ_HEADER) {                  // ── 시각 요청 ──
            if(buf.size() < REQ_LEN) return;
            if(!requestIsValid(buf)) {
                buf.erase(0, 1);
                continue;
            }
            // 실제 전송은 runOnce 루프가 한다 (위 g_time_requested 주석 참조)
            g_time_requested = true;
            buf.erase(0, REQ_LEN);
            continue;
        }

        buf.erase(0, 1);   // 알 수 없는 바이트 — 헤더를 만날 때까지 버린다
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
// 현재 벽시계(로컬 타임)를 5바이트로 만들어 FFE1 에 write 한다.
// write_command(=write without response)를 쓴다 — HM-10 투과모드는 응답을
// 돌려주지 않으므로 write_request 로 보내면 확인 응답을 기다리다 타임아웃 난다.
bool sendTimeSync(SimpleBLE::Peripheral& peripheral) {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    if(!localtime_r(&now, &lt)) {
        // 조용히 빠져나가면 '왜 안 보내지'를 영영 알 수 없다
        std::cerr << "[Relay Node] 시각 변환 실패 (localtime_r)" << std::endl;
        return false;
    }

    uint8_t pkt[TIME_PKT_LEN];
    pkt[0] = TIME_PKT_HEADER;
    pkt[1] = (uint8_t)lt.tm_hour;
    pkt[2] = (uint8_t)lt.tm_min;
    pkt[3] = (uint8_t)lt.tm_sec;

    uint8_t sum = 0;
    for(size_t i = 0; i + 1 < TIME_PKT_LEN; i++) sum ^= pkt[i];
    pkt[TIME_PKT_LEN - 1] = sum;

    try {
        peripheral.write_command(SVC_FFE0, CHAR_FFE1,
                                 SimpleBLE::ByteArray(pkt, pkt + TIME_PKT_LEN));
    } catch (const std::exception& e) {
        std::cerr << "[Relay Node] 시각 전송 실패: " << e.what() << std::endl;
        return false;
    }

    // 파일 전체가 std::cout + std::endl 이라 여기도 맞춘다.
    // printf 는 stdout 이 파이프에 물리면 버퍼에 남아 순서가 뒤엉킬 수 있다.
    char hhmmss[16];
    std::snprintf(hhmmss, sizeof(hhmmss), "%02d:%02d:%02d",
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    std::cout << "[Relay Node] 시각 동기 전송 " << hhmmss << std::endl;
    return true;
}

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
    g_time_requested = false;   // 끊기기 직전 요청이 남아 있으면 새 세션에서 헛 전송이 된다

    // client 만 캡처 — main 지역변수라 이 함수보다 오래 살고, 버퍼는 전역이라 불필요
    peripheral.notify(SVC_FFE0, CHAR_FFE1, [&client](SimpleBLE::ByteArray bytes) {
        if(g_cfg.debug_hex) {
            std::cout << "[Relay Node] raw " << bytes.size() << "B: " << toHex(bytes) << std::endl;
        }
        g_rx_buffer.append(bytes.begin(), bytes.end());
        consumeBuffer(g_rx_buffer, client);
    });

    // 연결 직후 곧바로 한 번 — 웨어러블이 재부팅했다면 빌드 시각으로 떠 있다.
    sendTimeSync(peripheral);
    auto last_sync = std::chrono::steady_clock::now();

    while(peripheral.is_connected() && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 웨어러블이 물어보면 그 자리에서 답한다 (재부팅 직후가 대부분이다).
        // exchange 로 꺼내야 요청 하나에 한 번만 응답한다.
        if(g_time_requested.exchange(false)) {
            sendTimeSync(peripheral);
            last_sync = std::chrono::steady_clock::now();   // 방금 보냈으니 주기도 리셋
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync).count()
               >= TIME_SYNC_MS) {
            last_sync = now;
            sendTimeSync(peripheral);
        }
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
    // SIGTERM 도 등록 — systemctl stop 이 쓰는 신호, 안 잡으면 정리 없이 즉사
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // 종료 감시 — 신호 후 3초 안에 안 끝나면 강제 종료
    // 멎는 지점이 SimpleBLE·mosquitto 내부라 우리가 못 고침
    // 신호 직후부터 재야 함 — 루프 밖에서 재면 루프 안에서 멎었을 때 도달 불가
    std::thread([] {
        while(g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cerr << "[Relay Node] 정리가 3초 넘게 안 끝나 강제 종료" << std::endl;
        std::_Exit(1);
    }).detach();

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
    std::cout << "[Relay Node] 종료" << std::endl;
    return 0;
}
