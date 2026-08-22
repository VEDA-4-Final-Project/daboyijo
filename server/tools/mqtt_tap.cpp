// MQTT 도청기 — "내가 쏜 게 브로커에 실제로 들어갔나"를 눈으로 확인하는 진단 도구
//
// 발행하는 쪽은 publish 가 성공해도 그게 구독자에게 닿았는지 알 수 없다(QoS 0).
// 그래서 받는 쪽이 조용할 때 보내는 쪽 탓인지 받는 쪽 탓인지 가려면, 제3자로
// 붙어서 직접 들어보는 수밖에 없다.
//
// 서버(MqttMasterManager)와 같은 조건으로 붙여보는 용도이기도 하다:
//   ./mqtt_tap --broker dabo.local --port 8883 --ca ../../MQTT/certs/ca.crt
// 이게 실패하면 서버가 못 붙는 것과 같은 원인이다 — 재현된 것이니 여기서 고치면 된다.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "MqttClient_veda.hpp"

namespace {
std::atomic<bool> g_running{true};
std::atomic<int>  g_count{0};
void onSignal(int) { g_running = false; }
}  // namespace

int main(int argc, char** argv) {
    std::string broker = "127.0.0.1";
    int         port   = 1883;
    std::string ca_path;
    std::string topic  = "veda/wearable/data";
    int         seconds = 0;          // 0 = Ctrl+C 까지 계속

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << a << " 값이 없다\n"; std::exit(1); }
            return argv[++i];
        };
        if      (a == "--broker")  broker  = next();
        else if (a == "--port")    port    = std::atoi(next().c_str());
        else if (a == "--ca")      ca_path = next();
        else if (a == "--topic")   topic   = next();
        else if (a == "--seconds") seconds = std::atoi(next().c_str());
        else if (a == "-h" || a == "--help") {
            std::cout << "사용법: mqtt_tap [--broker H] [--port P] [--ca PATH] "
                         "[--topic T] [--seconds N]\n";
            return 0;
        } else { std::cerr << "모르는 옵션: " << a << "\n"; return 1; }
    }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // client id 는 브로커 안에서 유일해야 한다 — 겹치면 나중 접속이 먼저 것을 끊는다.
    // 진단 중에 서버를 끊어버리면 원인 찾기가 더 헷갈려지므로 pid 를 붙인다.
    MqttClient_veda client("mqtt_tap_" + std::to_string(getpid()));
    if (!ca_path.empty()) client.setTlsConfig(ca_path);

    client.setCallback([&](const std::string& t, const std::string& p) {
        std::cout << "[받음 " << ++g_count << "] " << t << "  " << p << std::endl;
    });
    client.subscribeTopic(topic);

    std::cout << "[mqtt_tap] " << broker << ":" << port
              << (ca_path.empty() ? " (평문)" : " (TLS, ca=" + ca_path + ")")
              << " 로 접속 시도...\n";
    const bool ok = client.connectToBroker(broker, port);
    client.startLoop();
    if (!ok) std::cerr << "[mqtt_tap] 최초 연결 실패 — 재접속을 기다린다\n";

    std::cout << "[mqtt_tap] '" << topic << "' 구독 중. Ctrl+C 로 종료.\n";
    for (int t = 0; g_running && (seconds == 0 || t < seconds * 10); ++t)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "[mqtt_tap] 총 " << g_count.load() << "건 수신하고 종료한다\n";
    client.stopLoop();
    client.disconnect();
    return g_count.load() > 0 ? 0 : 2;   // 한 건도 못 받으면 실패로 끝낸다
}
