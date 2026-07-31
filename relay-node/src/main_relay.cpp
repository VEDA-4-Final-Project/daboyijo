// 중계 노드 — HM-10 BLE 수신 → MQTT 발행
// STM32 → HM-10 → BLE → RPi4(이 코드) → 브로커 → master_node
//
// 수신 패킷 5바이트 (firmware/App/Drivers/hm10.h 와 동일 스펙, 1Hz 주기)
//   [0] 0xAA  [1] 심박  [2] SpO2  [3] 체온(정수°C)  [4] 낙상 0/1

#include "MqttClient_veda.hpp"
#include <simpleble/SimpleBLE.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

const std::string DEVICE_ID   = "wearable_01";
const std::string TARGET_ADDR = "88:4a:ea:62:0b:03";   // 우리 HM-10 MAC (소문자)
const std::string SVC_FFE0    = "0000ffe0-0000-1000-8000-00805f9b34fb";
const std::string CHAR_FFE1   = "0000ffe1-0000-1000-8000-00805f9b34fb";
const std::string TOPIC       = "veda/wearable/data";

const uint8_t PKT_HEADER = 0xAA;
const size_t  PKT_LEN    = 5;
const int QOS_VITAL      = 0;   // 바이탈: 빠르게 (유실 감수)
const int QOS_FALL       = 1;   // 낙상: 반드시 전달
const int SCAN_MS        = 8000;
const int RECONNECT_MS   = 3000;

static std::atomic<bool> g_running{true};
static uint8_t g_prev_fall = 0;   // 낙상 상승엣지 판정용. BLE 콜백에서만 접근
static bool g_debug_hex = false;  // RELAY_DEBUG_HEX=1 이면 원시 바이트 덤프

void onSigint(int) { g_running = false; }

std::string toLower(std::string s) {
    for(char& c : s) c = std::tolower((unsigned char)c);
    return s;
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

// 체크섬이 없어서 필드 범위로 검증 — 하나라도 벗어나면 헤더 오정렬로 간주
bool packetIsValid(const std::string& buf) {
    return (uint8_t)buf[2] <= 100 && (uint8_t)buf[3] <= 100 && (uint8_t)buf[4] <= 1;
}

// 패킷 1개 → WearableData → JSON 발행
void publishPacket(uint8_t hr, uint8_t spo2, uint8_t temp, uint8_t fall, MqttClient_veda& client) {
    // 펌웨어가 낙상 플래그를 5초간 레벨로 유지하므로(HM10_FALL_HOLD_MS)
    // 그대로 흘리면 낙상 1건에 알람이 5~6번 뜬다. 상승엣지에서만 낙상으로 본다.
    bool rising_edge = (fall == 1 && g_prev_fall == 0);
    g_prev_fall = fall;

    WearableData data;
    data.device_id        = DEVICE_ID;
    data.heart_rate       = hr;
    data.spo2             = spo2;
    data.temperature      = temp;
    data.is_fall_detected = rising_edge;
    data.timestamp        = nowMs();   // 패킷에 시각 필드가 없어 수신 시점으로 찍는다

    nlohmann::json j = data;
    std::string payload = j.dump();

    client.publishMessage(TOPIC, payload, rising_edge ? QOS_FALL : QOS_VITAL);
    std::cout << "[Relay Node] Published " << (rising_edge ? "FALL : " : "vital: ") << payload << std::endl;
}

// 조각난 데이터를 0xAA 헤더 기준 5바이트로 재조립
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

        publishPacket(buf[1], buf[2], buf[3], buf[4], client);
        buf.erase(0, PKT_LEN);
    }
}

std::optional<SimpleBLE::Peripheral> findDevice(SimpleBLE::Adapter& adapter) {
    std::cout << "[Relay Node] Scanning for HM-10 (max " << SCAN_MS << "ms)..." << std::endl;
    std::optional<SimpleBLE::Peripheral> found;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        if(!found.has_value() && toLower(p.address()) == TARGET_ADDR) found = p;
    });

    adapter.scan_start();
    auto t0 = std::chrono::steady_clock::now();
    while(!found.has_value() && g_running &&
          std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(SCAN_MS)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if(adapter.scan_is_active()) adapter.scan_stop();

    return found;
}

// 연결 1회 세션 — 끊길 때까지 수신하고 정리하고 돌아온다
void runOnce(SimpleBLE::Adapter& adapter, MqttClient_veda& client) {
    auto dev = findDevice(adapter);
    if(!dev) {
        std::cout << "[Relay Node] HM-10(" << TARGET_ADDR << ") not found. retrying" << std::endl;
        return;
    }

    SimpleBLE::Peripheral peripheral = *dev;
    peripheral.connect();
    std::cout << "[Relay Node] Connected: " << peripheral.identifier() << ". subscribing FFE1" << std::endl;

    g_prev_fall = 0;   // 끊긴 사이 상태를 모르므로 엣지 판정 초기화
    std::string rx_buffer;

    peripheral.notify(SVC_FFE0, CHAR_FFE1, [&](SimpleBLE::ByteArray bytes) {
        if(g_debug_hex) {
            std::cout << "[Relay Node] raw " << bytes.size() << "B: " << toHex(bytes) << std::endl;
        }
        rx_buffer.append(bytes.begin(), bytes.end());
        consumeBuffer(rx_buffer, client);
    });

    while(peripheral.is_connected() && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 정리해야 다음에 다시 스캔·연결된다. 안 하면 HM-10 이 연결 상태로 남아
    // 광고를 멈추기 때문에 스캔에 안 잡힌다.
    try { peripheral.unsubscribe(SVC_FFE0, CHAR_FFE1); } catch (...) {}
    try { peripheral.disconnect(); } catch (...) {}
    std::cout << "[Relay Node] Disconnected (cleaned up)" << std::endl;
}

int main() {
    std::signal(SIGINT, onSigint);

    const char* dbg = std::getenv("RELAY_DEBUG_HEX");
    g_debug_hex = (dbg && std::string(dbg) != "0");

    MqttClient_veda client("Relay_Node_01");

    std::cout << "[Relay Node] Connecting to Mqtt broker..." << std::endl;
    if(!client.connectToBroker("localhost",1883)) {
        // 죽이지 않는다 — 오프라인 큐에 쌓다가 자동 재연결되면 flush 된다
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
        if(g_running) std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_MS));
    }

    client.stopLoop();
    return 0;
}
