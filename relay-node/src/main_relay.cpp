// 중계 노드 — HM-10 BLE 수신 → MQTT 발행
// STM32 → HM-10 → BLE → RPi4(이 코드) → 브로커 → master_node
//
// 수신 패킷 7바이트 (firmware/App/Drivers/hm10.h 와 동일 스펙)
//   [0] 0xAA  [1] 심박  [2] SpO2  [3] 낙상 0/1
//   [4] 걸음수 하위바이트  [5] 걸음수 상위바이트  (리틀엔디언 uint16)
//   [6] 체크섬 — [0]~[5] 의 XOR
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

// HM-10 투과모드가 쓰는 표준 UUID — 기기가 바뀌어도 같다
const std::string SVC_FFE0    = "0000ffe0-0000-1000-8000-00805f9b34fb";
const std::string CHAR_FFE1   = "0000ffe1-0000-1000-8000-00805f9b34fb";

const uint8_t PKT_HEADER = 0xAA;
const size_t  PKT_LEN    = 7;
const int QOS_VITAL      = 0;   // 바이탈: 빠르게 (유실 감수)
const int QOS_FALL       = 1;   // 낙상: 반드시 전달

// 기기마다 달라지는 값 — relay-node.conf 에서 읽는다
struct Config {
    std::string broker_host  = "localhost";
    int         broker_port  = 1883;
    std::string device_id    = "wearable_01";
    std::string topic        = "veda/wearable/data";
    std::string hm10_addr    = "88:4a:ea:62:0b:03";   // 소문자 MAC
    int         scan_ms      = 8000;
    int         reconnect_ms = 3000;
    bool        debug_hex    = false;                 // 원시 바이트 덤프
    // 브로커 검증용 CA 인증서. 비어 있으면 TLS 없이 평문으로 붙는다
    // (브로커의 1883 리스너가 살아 있는 동안의 폴백 겸, 롤백 손잡이).
    std::string ca_path      = "";
};

static Config g_cfg;
static std::atomic<bool> g_running{true};
static uint8_t g_prev_fall = 0;   // 낙상 상승엣지 판정용. notify 등록 전 초기화 후 BLE 콜백에서만 접근

// 조각난 notify 를 재조립하는 버퍼. runOnce 지역변수로 두면 안 되는데,
// 아래 notify 콜백이 이걸 참조로 붙잡은 채 BLE 스레드에서 돌기 때문이다.
// unsubscribe 가 실패하면(그 자리 catch(...) 가 삼킨다) 콜백이 등록된 채로 남아
// 죽은 스택을 건드린다. 수명을 프로세스와 맞춰 그 창을 아예 없앤다.
// g_prev_fall 과 마찬가지로 세션 시작 때 초기화하고 이후엔 BLE 콜백만 만진다.
static std::string g_rx_buffer;

void onSigint(int) { g_running = false; }

std::string toLower(std::string s) {
    for(char& c : s) c = std::tolower((unsigned char)c);
    return s;
}

// 실행 파일이 있는 디렉터리. 현재 디렉터리를 쓰면 어디서 실행했느냐에 따라
// 설정 파일을 못 찾는다 (systemd 는 작업 디렉터리가 / 다)
std::string exeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

// 설정 파일은 손으로 고치는 곳이라 오타가 나기 쉽다.
// 잘못된 값 하나로 죽지 않고 기본값을 유지한 채 어느 키가 문제인지 알린다.
int toInt(const std::string& k, const std::string& v, int fallback) {
    try {
        return std::stoi(v);
    } catch(const std::exception&) {
        std::cerr << "[설정] " << k << " 값이 숫자가 아님: \"" << v
                  << "\" - 기본값 " << fallback << " 사용" << std::endl;
        return fallback;
    }
}

// aa:bb:cc:dd:ee:ff 꼴인지만 본다. 틀리면 스캔에 영원히 안 잡히는데
// "not found" 만 반복돼서 설정 오타인지 기기가 꺼진 건지 구분이 안 된다.
bool looksLikeMac(const std::string& s) {
    if(s.size() != 17) return false;
    for(size_t i = 0; i < s.size(); i++) {
        bool sep = (i % 3 == 2);
        if(sep && s[i] != ':') return false;
        if(!sep && !std::isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// "키 = 값" 형식. # 는 주석. 없는 키는 기본값을 그대로 둔다
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

// 체크섬 검증 — 마지막 바이트는 앞선 전체의 XOR 이어야 한다.
// 프레이밍이 목적이다. 데이터 바이트에도 0xAA 가 나올 수 있어(걸음 수 170,
// 426, 682…) 헤더를 잘못 잡을 수 있는데, 이 검사가 그것을 즉시 잡아낸다.
bool checksumIsValid(const std::string& buf) {
    uint8_t x = 0;
    for(size_t i = 0; i < PKT_LEN - 1; i++) x ^= (uint8_t)buf[i];
    return x == (uint8_t)buf[PKT_LEN - 1];
}

// 필드 범위 검증. 체크섬이 프레이밍을 책임지므로 이제 이건 보조 수단이다 —
// 통신이 아니라 펌웨어 쪽 논리 오류(범위를 벗어난 값을 싣는 버그)를 잡는다.
// 심박은 0~255 전체가 유효값이라 검사할 수 없고, 0 은 "측정 불가" 라 통과시킨다.
bool fieldsAreSane(const std::string& buf) {
    return (uint8_t)buf[2] <= 100 && (uint8_t)buf[3] <= 1;
}

// 패킷 1개 → WearableData → JSON 발행
//
// 걸음 수는 아직 JSON 에 싣지 않는다. WearableData 는 server/Qt 가 함께 쓰는
// 공용 스키마라 필드를 늘리면 세 컴포넌트를 동시에 배포해야 한다.
// 우선 수신값을 로그로만 드러내 BLE 구간이 맞는지 확인할 수 있게 해 둔다.
void publishPacket(uint8_t hr, uint8_t spo2, uint8_t fall,
                   uint16_t steps, MqttClient_veda& client) {
    // 펌웨어가 낙상 플래그를 5초간 레벨로 유지하므로(HM10_FALL_HOLD_MS)
    // 그대로 흘리면 낙상 1건에 알람이 5~6번 뜬다. 상승엣지에서만 낙상으로 본다.
    bool rising_edge = (fall == 1 && g_prev_fall == 0);
    g_prev_fall = fall;

    WearableData data;
    data.device_id        = g_cfg.device_id;
    data.heart_rate       = hr;
    data.spo2             = spo2;
    // 체온은 펌웨어에서 제거됐다. WearableData 는 server/Qt 가 함께 쓰는 공용
    // 스키마라 필드를 지우면 세 컴포넌트를 동시에 배포해야 해서, 우선 0 으로
    // 채워 스키마 호환만 유지한다. 어차피 이전에도 항상 0 이었다.
    data.temperature      = 0.0;
    data.is_fall_detected = rising_edge;
    data.timestamp        = nowMs();   // 패킷에 시각 필드가 없어 수신 시점으로 찍는다

    nlohmann::json j = data;
    std::string payload = j.dump();

    client.publishMessage(g_cfg.topic, payload, rising_edge ? QOS_FALL : QOS_VITAL);
    std::cout << "[Relay Node] Published " << (rising_edge ? "FALL : " : "vital: ") << payload
              << " (steps=" << steps << ")" << std::endl;
}

// 조각난 데이터를 0xAA 헤더 기준 7바이트로 재조립
void consumeBuffer(std::string& buf, MqttClient_veda& client) {
    while(true) {
        size_t h = buf.find((char)PKT_HEADER);
        if(h == std::string::npos) { buf.clear(); return; }   // 헤더 없음
        if(h > 0) buf.erase(0, h);                            // 앞쪽 쓰레기 버림
        if(buf.size() < PKT_LEN) return;                      // 덜 모임 — 다음 notify 대기

        if(!checksumIsValid(buf)) {
            buf.erase(0, 1);   // 가짜 헤더 → 1바이트 밀고 재동기
            continue;
        }

        // 체크섬은 맞는데 값이 이상하다 — 프레이밍이 아니라 송신 측 문제다.
        // 조용히 버리면 원인을 영영 모르므로 남기고, 패킷은 버린다.
        if(!fieldsAreSane(buf)) {
            std::cerr << "[Relay Node] 체크섬은 유효한데 필드가 범위 밖 — 펌웨어 확인 필요"
                      << " (spo2=" << (int)(uint8_t)buf[2]
                      << " fall=" << (int)(uint8_t)buf[3] << ")" << std::endl;
            buf.erase(0, PKT_LEN);
            continue;
        }

        // 리틀엔디언 2바이트. char 가 부호 있는 플랫폼에서 상위비트가 부호확장되지
        // 않도록 반드시 uint8_t 로 캐스팅한 뒤 조립한다.
        uint16_t steps = (uint16_t)((uint8_t)buf[4] | ((uint16_t)(uint8_t)buf[5] << 8));

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

    // 위 콜백은 지역변수 found 를 참조로 붙잡고 있는데 어댑터에는 계속 등록된 채로 남는다.
    // 해제하지 않으면 다음 스캔까지의 사이에 콜백이 늦게 불릴 때 죽은 스택을 건드린다.
    adapter.set_callback_on_scan_found([](SimpleBLE::Peripheral){});

    return found;
}

// 연결 1회 세션 — 끊길 때까지 수신하고 정리하고 돌아온다
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
    g_rx_buffer.clear();   // 이전 세션의 반쪽 패킷을 물고 가지 않는다

    // client 만 참조로 잡는다 (main 의 지역변수라 이 함수보다 오래 산다).
    // 버퍼는 전역이라 캡처할 필요가 없고, 캡처하지 않는 편이 수명 문제도 안 생긴다.
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

    // 정리해야 다음에 다시 스캔·연결된다. 안 하면 HM-10 이 연결 상태로 남아
    // 광고를 멈추기 때문에 스캔에 안 잡힌다.
    // 실패를 삼키지 않고 알린다 — 해제가 안 됐다면 콜백이 아직 살아 있다는 뜻이라,
    // 다음 세션에서 이상하게 굴 때 여기 로그가 유일한 단서다.
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

    // MQTT 클라이언트 id 는 브로커 안에서 유일해야 한다. 같은 id 두 개가 붙으면
    // 나중에 온 쪽이 먼저 있던 쪽을 끊어버려서, 웨어러블을 두 대로 늘리는 순간
    // 중계 노드끼리 서로를 밀어낸다. 기기마다 다른 device_id 를 붙여 그걸 막는다.
    MqttClient_veda client("relay_" + g_cfg.device_id);

    // ca.crt 도 conf 와 같은 이유로 실행 파일 기준으로 푼다 — systemd 는 작업
    // 디렉터리가 / 라, 상대 경로 그대로 두면 손으로 실행할 때만 되고 서비스로
    // 띄우면 tls_set 이 실패한다. 그 실패는 "평문도 TLS 도 아닌" 미연결이 된다.
    if(!g_cfg.ca_path.empty() && g_cfg.ca_path[0] != '/')
        g_cfg.ca_path = exeDir() + "/" + g_cfg.ca_path;
    if(!g_cfg.ca_path.empty()) client.setTlsConfig(g_cfg.ca_path);

    std::cout << "[Relay Node] Connecting to Mqtt broker "
              << g_cfg.broker_host << ":" << g_cfg.broker_port
              << (g_cfg.ca_path.empty() ? " (평문)" : " (TLS)") << "..." << std::endl;
    if(!client.connectToBroker(g_cfg.broker_host, g_cfg.broker_port)) {
        // TLS 를 쓰는데 실패했다면 인증서 문제일 가능성이 높고, 그건 저절로 낫지
        // 않는다. tls_set 이 실패하면 mosquitto_connect 가 호출조차 안 돼 자동
        // 재연결이 돌지 않고, 설정이 핸들에 안 올라갔으니 나중에 붙어봐야 평문으로
        // 8883 을 두드릴 뿐이다. 그대로 두면 BLE 는 멀쩡히 돌면서 패킷은 오프라인
        // 큐에만 무한정 쌓이는 "조용히 죽은" 상태가 된다 — 차라리 끊고 systemd 가
        // 다시 띄우게 한다.
        if(!g_cfg.ca_path.empty()) {
            std::cerr << "[Relay Node] TLS 연결 실패 — ca_path 확인: " << g_cfg.ca_path
                      << " (종료, systemd 가 재시작한다)" << std::endl;
            return 1;
        }
        // 평문일 때는 기존대로 버틴다 — 브로커가 늦게 떠도 자동 재연결로 붙고,
        // 그때 오프라인 큐가 flush 된다
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
