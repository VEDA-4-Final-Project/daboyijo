// 다보이조 중앙 서버 진입점.
// 현재 단계(2주차): RTSP 4채널 수신 확인 — 채널별 접속 상태와 수신 FPS를 출력한다.
// 이후 video/ 영상처리(저조도 보정·ROI 마스킹), core/ 교차 검증 룰엔진이 붙는다.

#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "frame_queue.hpp"
#include "rtsp_client.hpp"

namespace {

std::sig_atomic_t g_stop = 0;

void handleSignal(int) {
    g_stop = 1;
}

struct CameraConfig {
    int channel;
    std::string url;
};

// config/cameras.conf 파싱. 형식: "채널번호=RTSP URL", '#' 주석
std::vector<CameraConfig> loadConfig(const std::string& path) {
    std::vector<CameraConfig> cameras;
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "설정 파일을 열 수 없음: %s\n", path.c_str());
        return cameras;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        cameras.push_back({std::stoi(line.substr(0, eq)), line.substr(eq + 1)});
    }
    return cameras;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path =
        (argc > 1) ? argv[1] : "config/cameras.conf";

    auto cameras = loadConfig(config_path);
    if (cameras.empty()) {
        std::fprintf(stderr, "카메라 설정 없음. config/cameras.conf.example 참고\n");
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    FrameQueue queue(16);
    std::vector<std::unique_ptr<RtspClient>> clients;
    for (const auto& cam : cameras) {
        clients.push_back(
            std::make_unique<RtspClient>(cam.channel, cam.url, queue));
        clients.back()->start();
    }
    std::printf("%zu개 채널 수신 시작 (Ctrl+C로 종료)\n", clients.size());

    // TODO(video): 프레임 소비 → 저조도 보정, 침상 ROI 마스킹
    // TODO(core): WiseAI 메타데이터 + 웨어러블 신호 교차 검증
    auto last_report = std::chrono::steady_clock::now();
    std::vector<uint64_t> last_counts(clients.size(), 0);

    while (!g_stop) {
        auto frame = queue.pop(std::chrono::milliseconds(200));
        (void)frame;  // 아직 소비 로직 없음 — 큐 적체 방지용 drain

        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_report);
        if (elapsed.count() >= 5) {
            std::ostringstream status;
            for (size_t i = 0; i < clients.size(); ++i) {
                uint64_t count = clients[i]->frameCount();
                double fps =
                    static_cast<double>(count - last_counts[i]) / elapsed.count();
                status << "[ch" << clients[i]->channel() << "] "
                       << (clients[i]->connected() ? "OK" : "끊김") << " "
                       << fps << "fps  ";
                last_counts[i] = count;
            }
            std::printf("%s\n", status.str().c_str());
            last_report = now;
        }
    }

    std::printf("종료 중...\n");
    for (auto& client : clients) {
        client->stop();
    }
    return 0;
}
