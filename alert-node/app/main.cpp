// 알림 노드 본체 — MQTT 로 AlarmCommand 를 받아 오디오(wm8960)·LED(hub75)로 출력
//
// 콜백은 큐에 넣고 바로 리턴 — 패널 스크롤이 십수 초라 콜백에서 직접 그리면
// 그동안 다음 메시지를 못 받음
//
// 빌드: make          실행: sudo ./alert-node [-c 설정파일]
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
#include "AudioReceiver.h"

namespace {

std::atomic<bool> running{true};
void onSignal(int) { running = false; }

// ---------------- 설정 ----------------

struct Config {
    std::string broker_host = "localhost";
    int         broker_port = 1883;
    std::string node_id     = "alarm_rpi_01";
    std::string topic       = "veda/alarm/control";
    std::string audio_dir   = "sounds";
    std::string idle_text   = "감시 중";      // 평상시 표시 — 64px 안에 들어갈 것
    int         matrix_passes = 3;            // 서버가 안 정하면 이만큼 흘림
    int         matrix_brightness = 128;      // 평상시(idle) 밝기 0~255 — 테스트는 이 값으로 복귀
    std::string ca_path     = "";             // 브로커 검증용 CA — 비면 평문 폴백
    int         broadcast_port    = 5000;      // UDP 방송 수신 포트
    std::string alsa_device       = "hw:2,0";  // ALSA 재생 장치
};

// 실행 파일이 있는 디렉터리 — systemd 는 작업 디렉터리가 / 라 상대 경로가 깨짐
std::string exeDir()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

// 오타 하나로 죽지 않게 — 기본값 유지하고 어느 키가 문제인지만 알림
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

// "키 = 값" 형식, # 는 주석 — 없는 키는 기본값 유지
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
        else if (k == "matrix_brightness") c.matrix_brightness = toInt(k, v, c.matrix_brightness);
        else if (k == "ca_path")     c.ca_path     = v;
        else if (k == "broadcast_port")   c.broadcast_port = toInt(k, v, c.broadcast_port); 
        else if (k == "alsa_device")      c.alsa_device = v;
    }
}

// ---------------- 명령 해석 ----------------

severity toSeverity(const std::string& type)
{
    if (type == "FALL" || type == "EGRESS") return SEV_CRIT;
    if (type == "VITAL_ABNORMAL")           return SEV_WARN;
    // CONTROL(관제 앱 "테스트")도 주황 — Qt 미리보기와 색을 맞춘다(둘 다 SEV_WARN 색상).
    if (type == "CONTROL")                  return SEV_WARN;
    return SEV_INFO;
}

// 패널에 흘릴 문구 — 호실을 알면 노드가 조립, 모르면 서버 문구 사용
std::string toText(const AlarmCommand& c)
{
    // room 은 네트워크에서 온 값이라 범위를 안 믿음 — 보내는 쪽이 초기화를 빠뜨리면
    // 쓰레기 정수가 실려 옴 (실측: -589012800)
    // 그대로 띄우면 표시도 틀리고 자릿수만큼 스크롤이 길어져(10자리 = 13초)
    // 뒤따르는 진짜 경보가 밀림, 0 은 원래부터 "모름"
    if (c.room <= 0 || c.room > 9999) return c.message;

    // 64px 를 한 바퀴 흘리는 데 문구 길이만큼 시간이 듦
    // 바뀌는 정보(호실)를 맨 앞에 두고 나머지는 최소한으로
    const std::string room = std::to_string(c.room);
    if (c.type == "FALL")           return room + "호 낙상 발생";
    if (c.type == "EGRESS")         return room + "호 침상 이탈";
    if (c.type == "VITAL_ABNORMAL") return room + "호 생체 이상";
    return room + "호 " + c.message;
}

// 절대 경로면 그대로, 파일명만 오면 audio_dir 에서 찾음
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



    // 아래 STOP 처리 때문에 재생기를 MQTT 스레드와 메인 루프가 같이 만짐
    // stop() 이 재생 스레드를 join 해서 동시에 부르면 같은 스레드를 두 번 join 함
    // 재생기 호출은 전부 이 뮤텍스를 잡고 함
    std::mutex player_mutex;

    // --- 방송 수신 객체 초기화 및 실행 ---
    AudioReceiver broadcastReceiver;

    // 긴급 방송 시작 전: 기존 사이렌/WAV 즉시 중단 및 ALSA 점유 해제
    broadcastReceiver.setPreemptCallback([&]() {
        printf("[방송]방송 수신 -> 기존 오디오 점유 해제\n");
        std::lock_guard<std::mutex> lk(player_mutex);
        player.stop();
    });

    broadcastReceiver.setRestoreCallback([&]() {
        printf("[방송]방송 수신 종료\n");
    });

    // 백그라운드 스레드에서 UDP 수신 대기 시작
    broadcastReceiver.start(cfg.broadcast_port, cfg.alsa_device);

    // 평상시(idle) 밝기·음량 = 관제 앱 "적용"으로 커밋된 값. 테스트(matrix=SHOW)는 잠깐
    // 이 값을 벗어나 보여주고, 스크롤이 끝나면 이 값으로 되돌아온다 — 적용해야만 유지된다.
    //   밝기 시작값: conf 의 matrix_brightness (시작 시 확정해 idle 을 예측 가능하게)
    //   음량 시작값: 하드웨어 믹서의 현재 값, 못 읽으면 50 폴백
    int baseBrightness = cfg.matrix_brightness;
    display.setBrightness(baseBrightness);
    int baseVolume = 50;
    { long v = player.getVolume(); if (v > 0) baseVolume = static_cast<int>(v); }

    MqttClient_veda client(cfg.node_id);

    // 온라인 상태 — 관제 앱이 "대상 노드" 배지에 쓴다. retain 을 켜서 관제 앱이 이 노드보다
    // 늦게 켜져도(구독 시점에) 마지막 상태를 바로 받는다.
    //   Will(연결 끊기 전 등록) : 브로커가 죽음/케이블 뽑힘 등 "비정상" 단절을 감지하면 대신 발행
    //   정상 종료 시 아래에서 "offline" 을 직접 한 번 더 보낸다 — clean disconnect 는 Will 이
    //   발동하지 않기 때문(Ctrl+C 로 끈 것도 배지에 정확히 반영되도록)
    const std::string statusTopic = "veda/alarm/" + cfg.node_id + "/status";
    client.setWill(statusTopic, "offline", 1, true);

    client.setCallback([&](const std::string&, const std::string& payload) {
        try {
            auto cmd = nlohmann::json::parse(payload).get<AlarmCommand>();
            if (cmd.target_device != cfg.node_id) return;

            // 소리 끄기만 큐를 안 거침 — 메인 루프가 스크롤에 붙잡혀 있으면
            // 한 바퀴(5~6초) 돌 때까지 사이렌이 계속 울림
            // 큐에도 넣어 매트릭스 CLEAR 등은 원래대로 처리 (두 번째 stop() 은 무해)
            if (cmd.audio_action == "STOP") {
                std::lock_guard<std::mutex> lk(player_mutex);
                player.stop();
            }
            queue.push(cmd);            // 오래 걸리는 일은 메인 루프에서
        } catch (const std::exception& e) {
            fprintf(stderr, "[MQTT] 파싱 실패: %s\n", e.what());
        }
    });

    // ca.crt 도 conf 와 같은 이유로 실행 파일 기준 — systemd 에선 상대 경로가 깨져
    // tls_set 이 실패하고 아래 종료 경로로 빠짐
    if (!cfg.ca_path.empty() && cfg.ca_path[0] != '/')
        cfg.ca_path = exeDir() + "/" + cfg.ca_path;
    if (!cfg.ca_path.empty()) client.setTlsConfig(cfg.ca_path);

    // 여기서 죽는 건 의도 — MqttClient_veda 가 재연결 시 구독을 다시 안 걸어서,
    // 최초 연결에 실패한 채 돌면 브로커가 나중에 떠도 영영 못 받음
    // 살아있는데 알림만 안 오는 상태가 제일 찾기 어려우므로 systemd 재시작에 맡김
    // MQTT_prod 에 재구독이 붙으면 중계 노드처럼 버티는 쪽으로 변경
    if (!client.connectToBroker(cfg.broker_host, cfg.broker_port)) {
        fprintf(stderr, "브로커 연결 실패: %s:%d%s - 종료 (systemd 가 재시작한다)\n",
                cfg.broker_host.c_str(), cfg.broker_port,
                cfg.ca_path.empty() ? "" : " [TLS]");
        return 1;
    }
    client.startLoop();
    // QoS 1 로 구독 — 실제 등급은 발행·구독 중 낮은 쪽이라 여기가 0 이면
    // 보내는 쪽이 1 로 보내도 마지막 구간에서 0 으로 깎임
    client.publish(statusTopic, "online", 1, true);   // retain — 아직 연결 전이면 큐에 담겼다 뒤에 나간다
    if (!client.subscribeTopic(cfg.topic, 1)) {
        fprintf(stderr, "[MQTT] 구독 요청 실패: %s - 알람을 못 받는다\n", cfg.topic.c_str());
    }
    printf("[알림노드] %s 로 %s 구독 시작 (node_id=%s)\n",
           cfg.broker_host.c_str(), cfg.topic.c_str(), cfg.node_id.c_str());

    // 표시 정책은 노드가 정함 — 서버는 발행 시점에 뒤에 뭐가 올지 모름
    //   최소 한 바퀴는 끝까지 — 안 그러면 연달아 올 때 앞의 것이 한 프레임만 스침
    //   그 뒤에 대기 중인 게 있으면 넘어감, 우선순위는 없음
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

        // "커밋할지"와 "끝나고 되돌릴지"는 서로 다른 질문이라 따로 판단한다.
        //   커밋(shouldCommit) — 관제 앱 "적용"(matrix_action=NONE, 화면에 아무것도 안
        //     띄우는 순수 설정 명령)일 때만 평상시(base) 값을 갱신한다. 실제 알람과
        //     "테스트"는 둘 다 matrix_action=SHOW 라 여기서 자동으로 제외된다 — 알람
        //     한 번 울렸다고 그 볼륨이 평상시 값으로 눌어붙으면 안 되기 때문.
        //   되돌림(shouldRevert) — 관제 앱 "테스트"(cmd.is_test)일 때만, 보여준 뒤
        //     평상시 값으로 되돌린다. 실제 알람은 여기 해당 안 됨 — loop=true 로 계속
        //     재생 중인데 볼륨을 되돌리면 사이렌이 저절로 조용해지는 사고가 난다.
        const bool shouldCommit = (cmd.matrix_action != "SHOW");
        const bool shouldRevert = cmd.is_test;

        // 밝기: 이 명령 동안 적용, 커밋 대상이면 평상시 밝기로 승격.
        if (cmd.brightness > 0) {
            display.setBrightness(cmd.brightness);
            if (shouldCommit) baseBrightness = cmd.brightness;
        }

        // 음량: 이번 재생에 쓸 값 결정. 커밋 대상이면 믹서에 바로 반영하고 base 로 승격.
        int playVolume = baseVolume;
        if (cmd.volume > 0) {
            playVolume = cmd.volume;
            if (shouldCommit) {
                std::lock_guard<std::mutex> lk(player_mutex);
                player.setVolume(cmd.volume);   // 적용: 소리 없이도 믹서에 즉시 반영
                baseVolume = cmd.volume;
            }
        }

        if (cmd.audio_action == "PLAY") {
            std::lock_guard<std::mutex> lk(player_mutex);
            player.setVolume(playVolume);   // 테스트=그 음량 / 이벤트=평상시 음량
            player.playWav(toAudioPath(cfg, cmd.audio_file), cmd.loop);   // 비동기
        } else if (cmd.audio_action == "STOP") {
            std::lock_guard<std::mutex> lk(player_mutex);
            player.stop();   // 대개 MQTT 콜백이 이미 껐다 — 두 번째 호출은 무해
        }

        if (cmd.matrix_action == "SHOW") {
            // 서버가 지정하면 그대로, 안 주면 노드 기본값
            int passes = cmd.matrix_passes > 0 ? cmd.matrix_passes : cfg.matrix_passes;
            severity sev = toSeverity(cmd.type);
            display.blinkCue(sev);              // 새 경보라는 신호
            display.show(toText(cmd), sev, passes, aborted);
        }
        else if (cmd.matrix_action == "CLEAR")
            display.clear();

        // "테스트" 스크롤이 끝나면 평상시(base) 밝기·음량으로 복귀 —
        // 테스트로 올린 값이 idle "감시 중" 에 눌어붙지 않게(적용해야만 유지된다).
        // 실제 알람(shouldRevert=false)은 여기 안 타므로, loop=true 로 재생 중인 사이렌의
        // 볼륨을 스크롤이 끝났다고 되돌리는 일이 없다.
        if (shouldRevert) {
            display.setBrightness(baseBrightness);
            std::lock_guard<std::mutex> lk(player_mutex);
            player.setVolume(baseVolume);
        }
    }

    // 정상 종료는 clean disconnect 라 Will 이 안 나간다 — 직접 offline 을 알린다.
    // stopLoop() 전에 보내야 실제로 소켓에 나간다(멈추면 아무도 안 보낸다).
    client.publish(statusTopic, "offline", 1, true);
    client.stopLoop();   // 먼저 멈춰야 아래 정리 중에 콜백이 끼어들지 않는다
    {
        std::lock_guard<std::mutex> lk(player_mutex);
        player.stop();
        broadcastReceiver.stop(); // 방송 수신 스레드 안전 종료
    }
    display.clear();
    printf("\n종료\n");
    return 0;
}
