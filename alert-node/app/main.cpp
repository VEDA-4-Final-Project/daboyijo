/*
 * main.cpp - 알림 노드 본체
 *
 * MQTT 로 AlarmCommand 를 받아 오디오(wm8960)와 LED 패널(hub75)로 내보낸다.
 *
 * 콜백은 큐에 넣고 바로 리턴한다. 패널 스크롤이 십수 초 걸려서, 콜백에서
 * 직접 그리면 그동안 다음 메시지를 못 받는다.
 *
 * 빌드: make          실행: sudo ./alert-node [-c 설정파일]
 */
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <linux/limits.h>

#include "MqttClient_veda.hpp"
#include "VedaAudioPlayer.hpp"
#include "alert-display.hpp"

namespace {

std::atomic<bool> running{true};
void onSignal(int) { running = false; }

/* ---------------- 설정 ---------------- */

struct Config {
    std::string broker_host = "localhost";
    int         broker_port = 1883;
    std::string node_id     = "alarm_rpi_01";
    std::string topic       = "veda/alarm/control";
    std::string audio_dir   = "sounds";
    std::string idle_text   = "감시 중";      // 평상시 표시 (64px 안에 들어갈 것)
};

/* 실행 파일이 있는 디렉터리. 현재 디렉터리를 쓰면 어디서 실행했느냐에 따라
 * 설정 파일을 못 찾는다 (systemd 는 작업 디렉터리가 / 다) */
std::string exeDir()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

/* "키 = 값" 형식. # 는 주석. 없는 키는 기본값을 그대로 둔다 */
void loadConfig(const std::string& path, Config& c)
{
    std::ifstream f(path);
    if (!f) {
        printf("[설정] %s 없음 - 기본값으로 진행\n", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        auto trim = [](std::string s) {
            size_t b = s.find_first_not_of(" \t\r");
            size_t e = s.find_last_not_of(" \t\r");
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));

        if      (k == "broker_host") c.broker_host = v;
        else if (k == "broker_port") c.broker_port = std::stoi(v);
        else if (k == "node_id")     c.node_id     = v;
        else if (k == "topic")       c.topic       = v;
        else if (k == "audio_dir")   c.audio_dir   = v;
        else if (k == "idle_text")   c.idle_text   = v;
    }
}

/* ---------------- 명령 해석 ---------------- */

severity toSeverity(const std::string& type)
{
    if (type == "FALL" || type == "EGRESS") return SEV_CRIT;
    if (type == "VITAL_ABNORMAL")           return SEV_WARN;
    return SEV_INFO;                        // CONTROL 등
}

/* 패널에 흘릴 문구. 호실을 알면 노드가 조립하고, 모르면 서버 문구를 쓴다 */
std::string toText(const AlarmCommand& c)
{
    if (c.room.empty()) return c.message;

    if (c.type == "FALL")           return "긴급 " + c.room + "호 낙상 발생 즉시 확인 요망";
    if (c.type == "EGRESS")         return "긴급 " + c.room + "호 침상 이탈 감지";
    if (c.type == "VITAL_ABNORMAL") return "주의 " + c.room + "호 " + c.message;
    return c.room + "호 " + c.message;
}

/* 서버가 절대 경로를 보내면 그대로, 파일명만 보내면 audio_dir 에서 찾는다 */
std::string toAudioPath(const Config& cfg, const std::string& file)
{
    if (file.empty() || file[0] == '/') return file;
    return exeDir() + "/" + cfg.audio_dir + "/" + file;
}

} // namespace

int main(int argc, char* argv[])
{
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    Config cfg;
    std::string confPath = exeDir() + "/alert-node.conf";
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) confPath = argv[2];
    loadConfig(confPath, cfg);

    AlertDisplay display;
    if (!display.open())
        fprintf(stderr, "[경고] hub75 패널 못 찾음 - 오디오만 동작한다\n");

    VedaAudioPlayer player;
    ThreadSafeQueue<AlarmCommand> queue;

    MqttClient_veda client(cfg.node_id);
    client.setCallback([&](const std::string&, const std::string& payload) {
        try {
            auto cmd = nlohmann::json::parse(payload).get<AlarmCommand>();
            if (cmd.target_device == cfg.node_id)
                queue.push(cmd);            // 오래 걸리는 일은 메인 루프에서
        } catch (const std::exception& e) {
            fprintf(stderr, "[MQTT] 파싱 실패: %s\n", e.what());
        }
    });

    if (!client.connectToBroker(cfg.broker_host, cfg.broker_port)) {
        fprintf(stderr, "브로커 연결 실패: %s:%d\n",
                cfg.broker_host.c_str(), cfg.broker_port);
        return 1;
    }
    client.startLoop();
    client.subscribeTopic(cfg.topic, 0);
    printf("[알림노드] %s 로 %s 구독 시작 (node_id=%s)\n",
           cfg.broker_host.c_str(), cfg.topic.c_str(), cfg.node_id.c_str());

    /* 스크롤 도중 새 명령이 오면 끊고 최신 것을 띄운다 */
    auto aborted = [&] { return !running || !queue.empty(); };
    bool idleShown = false;

    while (running) {
        AlarmCommand cmd;
        if (!queue.tryPop(cmd)) {
            if (!idleShown) { display.showStatic(cfg.idle_text, SEV_INFO); idleShown = true; }
            usleep(100000);
            continue;
        }
        idleShown = false;
        printf("[명령] type=%s room=%s audio=%s matrix=%s\n", cmd.type.c_str(),
               cmd.room.c_str(), cmd.audio_action.c_str(), cmd.matrix_action.c_str());

        if (cmd.audio_action == "PLAY") {
            player.setVolume(cmd.volume);
            player.playWav(toAudioPath(cfg, cmd.audio_file), cmd.loop);   // 비동기
        } else if (cmd.audio_action == "STOP") {
            player.stop();
        }

        if (cmd.brightness > 0) display.setBrightness(cmd.brightness);

        if (cmd.matrix_action == "SHOW")
            display.show(toText(cmd), toSeverity(cmd.type), cmd.matrix_passes, aborted);
        else if (cmd.matrix_action == "CLEAR")
            display.clear();
    }

    client.stopLoop();
    player.stop();
    display.clear();
    printf("\n종료\n");
    return 0;
}
