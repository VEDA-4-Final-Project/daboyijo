// 다보이조 중앙 서버 진입점.
// 파이프라인: RTSP 4채널 수신(libav) → [영상] 리사이즈→JPEG→Qt 송출
//                                    → [메타] WiseAI 객체감지 XML 파싱→감지 저장
// 5초마다 채널별 fps·사람 수·CPU·온도를 출력한다.
// 이후 저조도 보정·ROI 마스킹(video/), TLS 전송·교차 검증(core/)이 붙는다.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "detection.hpp"
#include "frame_queue.hpp"
#include "protocol/video_stream.h"
#include "rtsp_av_client.hpp"
#include "stream_server.hpp"
#include "system_stats.hpp"

namespace {

std::sig_atomic_t g_stop = 0;

void handleSignal(int) {
    g_stop = 1;
}

struct CameraConfig {
    int channel;
    std::string url;
};

struct ServerConfig {
    std::vector<CameraConfig> cameras;
    int stream_port = DBJ_VS_PORT_DEFAULT;
};

// 앞뒤 공백·탭·CR(윈도우 줄바꿈) 제거 — URL에 섞이면 RTSP 요청이 깨진다(505 등)
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(ws) - begin + 1);
}

// config/cameras.conf 파싱.
// 형식: "채널번호=RTSP URL" 또는 "stream_port=포트", '#' 주석
ServerConfig loadConfig(const std::string& path) {
    ServerConfig config;
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "설정 파일을 열 수 없음: %s\n", path.c_str());
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        if (key == "stream_port") {
            config.stream_port = std::stoi(value);
            continue;
        }
        if (key.empty() ||
            !std::all_of(key.begin(), key.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
            std::fprintf(stderr, "경고: 알 수 없는 설정 무시: %s\n", line.c_str());
            continue;
        }
        if (value.find(' ') != std::string::npos) {
            std::fprintf(stderr, "경고: URL에 공백 포함됨 (오타?): %s\n",
                         line.c_str());
        }
        config.cameras.push_back({std::stoi(key), std::move(value)});
    }
    return config;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path =
        (argc > 1) ? argv[1] : "config/cameras.conf";

    auto config = loadConfig(config_path);
    if (config.cameras.empty()) {
        std::fprintf(stderr, "카메라 설정 없음. config/cameras.conf.example 참고\n");
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    StreamServer stream_server(config.stream_port);
    if (!stream_server.start()) {
        return 1;
    }

    // 채널별 최신 감지 결과 저장소 (메타데이터 콜백 스레드 ↔ 메인 스레드 공유).
    // 교차 검증 룰엔진(core)이 이후 여기서 사람 위치를 읽어간다.
    std::mutex det_mutex;
    std::map<int, std::vector<Detection>> latest_detections;
    std::map<int, uint64_t> det_updates;  // 채널별 갱신 횟수 (리포트용)

    FrameQueue queue(16);
    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        auto client = std::make_unique<RtspAvClient>(cam.channel, cam.url, queue);
        client->setDetectionCallback(
            [&](int ch, std::vector<Detection> dets) {
                std::lock_guard<std::mutex> lock(det_mutex);
                latest_detections[ch] = std::move(dets);
                det_updates[ch] += 1;
            });
        client->start();
        clients.push_back(std::move(client));
    }
    std::printf("%zu개 채널 수신 시작 (Ctrl+C로 종료)\n", clients.size());

    // B안 파이프라인: 4분할 뷰용으로 640x360 리사이즈 → JPEG 인코딩
    // → 접속한 관제 클라이언트(Qt)에 TCP 송출 (protocol/video_stream.h)
    // TODO(video): 리사이즈 전에 저조도 보정·침상 ROI 마스킹 삽입
    // TODO(core): TLS 적용, WiseAI 메타데이터 + 웨어러블 신호 교차 검증
    const cv::Size kViewSize(640, 360);
    const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};

    struct ChannelStats {
        uint64_t processed = 0;
        uint64_t bytes = 0;
    };
    std::map<int, ChannelStats> stats;
    SystemStats system_stats;
    double encode_ms_total = 0;
    uint64_t encode_count = 0;

    auto last_report = std::chrono::steady_clock::now();
    std::vector<uint64_t> last_counts(clients.size(), 0);

    while (!g_stop) {
        auto frame = queue.pop(std::chrono::milliseconds(200));
        if (frame) {
            auto t0 = std::chrono::steady_clock::now();

            cv::Mat small;
            cv::resize(frame->image, small, kViewSize);
            std::vector<unsigned char> jpeg;
            cv::imencode(".jpg", small, jpeg, kJpegParams);

            auto& ch = stats[frame->channel];
            ch.processed += 1;
            ch.bytes += jpeg.size();

            stream_server.broadcast(frame->channel, std::move(jpeg));
            encode_ms_total += std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
            encode_count += 1;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_report);
        if (elapsed.count() >= 5) {
            std::ostringstream status;
            for (size_t i = 0; i < clients.size(); ++i) {
                int id = clients[i]->channel();
                uint64_t count = clients[i]->frameCount();
                double in_fps =
                    static_cast<double>(count - last_counts[i]) / elapsed.count();
                double out_fps =
                    static_cast<double>(stats[id].processed) / elapsed.count();

                // 이 채널의 최신 감지에서 사람 수 집계
                int humans = 0;
                {
                    std::lock_guard<std::mutex> lock(det_mutex);
                    for (const auto& d : latest_detections[id]) {
                        if (d.isHuman()) ++humans;
                    }
                }
                char buf[112];
                std::snprintf(buf, sizeof(buf),
                              "[ch%d] %s in %.1f out %.1ffps 사람%d  ",
                              id, clients[i]->connected() ? "OK" : "끊김",
                              in_fps, out_fps, humans);
                status << buf;
                last_counts[i] = count;
                stats[id] = ChannelStats{};
            }

            double avg_encode =
                encode_count ? encode_ms_total / encode_count : 0;
            char sys_buf[96];
            std::snprintf(sys_buf, sizeof(sys_buf),
                          "| CPU %.0f%% %.1f°C 인코딩 %.1fms 클라 %zu",
                          system_stats.cpuPercent(),
                          SystemStats::socTemperature(), avg_encode,
                          stream_server.clientCount());
            status << sys_buf;
            encode_ms_total = 0;
            encode_count = 0;

            std::printf("%s\n", status.str().c_str());
            last_report = now;
        }
    }

    std::printf("종료 중...\n");
    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    return 0;
}
