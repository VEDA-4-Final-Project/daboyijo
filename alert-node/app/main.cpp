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
#include <mutex>
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
    int         matrix_passes = 3;            // 서버가 안 정하면 이만큼 흘린다
    // 브로커 검증용 CA 인증서. 비어 있으면 TLS 없이 평문으로 붙는다
    // (브로커의 1883 리스너가 살아 있는 동안의 폴백 겸, 롤백 손잡이).
    std::string ca_path     = "";
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

/* 설정 파일은 손으로 고치는 곳이라 오타가 나기 쉽다.
 * 잘못된 값 하나로 죽지 않고 기본값을 유지한 채 어느 키가 문제인지 알린다. */
int toInt(const std::string& k, const std::string& v, int fallback)
{
    try {
        return std::stoi(v);
    } catch (const std::exception&) {
        fprintf(stderr, "[설정] %s 값이 숫자가 아님: \"%s\" - 기본값 %d 사용\n",
                k.c_str(), v.c_str(), fallback);
        return fallback;
    }
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
        else if (k == "broker_port") c.broker_port = toInt(k, v, c.broker_port);
        else if (k == "node_id")     c.node_id     = v;
        else if (k == "topic")       c.topic       = v;
        else if (k == "audio_dir")   c.audio_dir   = v;
        else if (k == "idle_text")   c.idle_text   = v;
        else if (k == "matrix_passes") c.matrix_passes = toInt(k, v, c.matrix_passes);
        else if (k == "ca_path")     c.ca_path     = v;
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
    /* room 은 int 다. 0(또는 음수)이면 호실을 특정할 수 없다는 뜻이라
     * 예전의 빈 문자열과 같게 취급해 서버 문구를 그대로 띄운다. */
    if (c.room <= 0) return c.message;
    const std::string room = std::to_string(c.room);

    /* 64px 화면을 한 바퀴 흘리는 데 문구 길이만큼 시간이 든다.
     * 바뀌는 정보(호실)를 맨 앞에 두고 나머지는 최소한으로 줄인다. */
    if (c.type == "FALL")           return room + "호 낙상 발생";
    if (c.type == "EGRESS")         return room + "호 침상 이탈";
    if (c.type == "VITAL_ABNORMAL") return room + "호 " + c.message;
    return room + "호 " + c.message;
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

    /* 아래 STOP 처리 때문에 재생기를 MQTT 스레드와 메인 루프가 같이 만진다.
     * VedaAudioPlayer::stop() 은 재생 스레드를 join 하므로 두 곳에서 동시에
     * 부르면 같은 스레드를 두 번 join 하게 된다. 재생기 호출은 전부 이걸 잡고 한다. */
    std::mutex player_mutex;

    MqttClient_veda client(cfg.node_id);
    client.setCallback([&](const std::string&, const std::string& payload) {
        try {
            auto cmd = nlohmann::json::parse(payload).get<AlarmCommand>();
            if (cmd.target_device != cfg.node_id) return;

            /* 소리 끄기만 큐를 거치지 않는다. 메인 루프가 패널 스크롤에 붙잡혀
             * 있으면 한 바퀴(문구 길이에 따라 5~6초)를 다 돈 뒤에야 큐를 보는데,
             * 그동안 사이렌이 계속 울린다. 관제사가 해제를 누른 순간 꺼져야 한다.
             * 큐에도 그대로 넣어 매트릭스 CLEAR 등 나머지 처리는 원래대로 간다
             * (메인 루프의 stop() 은 두 번째라 아무 일도 안 한다). */
            if (cmd.audio_action == "STOP") {
                std::lock_guard<std::mutex> lk(player_mutex);
                player.stop();
            }
            queue.push(cmd);            // 오래 걸리는 일은 메인 루프에서
        } catch (const std::exception& e) {
            fprintf(stderr, "[MQTT] 파싱 실패: %s\n", e.what());
        }
    });

    /* ca.crt 도 conf 와 같은 이유로 실행 파일 기준으로 푼다 — systemd 는 작업
     * 디렉터리가 / 라, 상대 경로 그대로 두면 손으로 실행할 때만 되고 서비스로
     * 띄우면 tls_set 이 실패해 아래 종료 경로로 빠진다. */
    if (!cfg.ca_path.empty() && cfg.ca_path[0] != '/')
        cfg.ca_path = exeDir() + "/" + cfg.ca_path;
    if (!cfg.ca_path.empty()) client.setTlsConfig(cfg.ca_path);

    /* 여기서 죽는 건 의도다. MqttClient_veda 는 재연결 시 구독을 다시 걸지 않아서,
     * 최초 연결에 실패한 채로 계속 돌면 브로커가 나중에 떠도 영영 아무것도 못 받는다.
     * 프로세스는 살아 있는데 알림만 안 오는 상태가 제일 발견하기 어려우므로,
     * 차라리 종료하고 systemd(Restart=always)가 다시 띄우게 둔다.
     * MQTT/MQTT_prod 쪽에 재구독이 붙으면 중계 노드처럼 버티는 쪽으로 바꿀 것. */
    if (!client.connectToBroker(cfg.broker_host, cfg.broker_port)) {
        fprintf(stderr, "브로커 연결 실패: %s:%d%s - 종료 (systemd 가 재시작한다)\n",
                cfg.broker_host.c_str(), cfg.broker_port,
                cfg.ca_path.empty() ? "" : " [TLS]");
        return 1;
    }
    client.startLoop();
    /* QoS 1 로 구독한다. 실제 전달 등급은 발행 QoS 와 구독 QoS 중 낮은 쪽이라,
     * 여기가 0 이면 보내는 쪽이 1 로 보내도 마지막 구간에서 0 으로 깎인다. */
    if (!client.subscribeTopic(cfg.topic, 1)) {
        fprintf(stderr, "[MQTT] 구독 요청 실패: %s - 알람을 못 받는다\n", cfg.topic.c_str());
    }
    printf("[알림노드] %s 로 %s 구독 시작 (node_id=%s)\n",
           cfg.broker_host.c_str(), cfg.topic.c_str(), cfg.node_id.c_str());

    /* 표시 정책은 노드가 정한다. 서버는 발행 시점에 그 뒤로 무엇이 올지
     * 모르기 때문이다.
     *   - 최소 한 바퀴는 끝까지 흘린다. 안 그러면 경보가 연달아 올 때
     *     앞의 것이 한 프레임만 스치고 사라진다.
     *   - 그 뒤에 대기 중인 게 있으면 넘어간다. 우선순위는 두지 않는다. */
    auto aborted = [&](int passesDone) {
        return !running || (passesDone >= 1 && !queue.empty());
    };
    bool idleShown = false;

    while (running) {
        AlarmCommand cmd;
        if (!queue.tryPop(cmd)) {
            if (!idleShown) { display.showStatic(cfg.idle_text, SEV_INFO); idleShown = true; }
            usleep(100000);
            continue;
        }
        idleShown = false;
        printf("[명령] type=%s room=%d audio=%s matrix=%s\n", cmd.type.c_str(),
               cmd.room, cmd.audio_action.c_str(), cmd.matrix_action.c_str());

        if (cmd.audio_action == "PLAY") {
            std::lock_guard<std::mutex> lk(player_mutex);
            player.setVolume(cmd.volume);
            player.playWav(toAudioPath(cfg, cmd.audio_file), cmd.loop);   // 비동기
        } else if (cmd.audio_action == "STOP") {
            std::lock_guard<std::mutex> lk(player_mutex);
            player.stop();   // 대개 MQTT 콜백이 이미 껐다 — 두 번째 호출은 무해
        }

        if (cmd.brightness > 0) display.setBrightness(cmd.brightness);

        if (cmd.matrix_action == "SHOW") {
            /* 서버가 지정하면 존중하고, 안 주면 노드 기본값 */
            int passes = cmd.matrix_passes > 0 ? cmd.matrix_passes : cfg.matrix_passes;
            severity sev = toSeverity(cmd.type);
            display.blinkCue(sev);              // 새 경보라는 신호
            display.show(toText(cmd), sev, passes, aborted);
        }
        else if (cmd.matrix_action == "CLEAR")
            display.clear();
    }

    client.stopLoop();   // 먼저 멈춰야 아래 정리 중에 콜백이 끼어들지 않는다
    {
        std::lock_guard<std::mutex> lk(player_mutex);
        player.stop();
    }
    display.clear();
    printf("\n종료\n");
    return 0;
}
