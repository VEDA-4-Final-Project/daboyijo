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

#include "caregiver_detector.hpp"  // [추가] 보호사 색 감지기
#include "care_timer.hpp"          // [추가] 케어시간 세션 추적
#include "database.hpp"
#include "detection.hpp"
#include "fall_detector.hpp"
#include "frame_queue.hpp"
#include "pose_estimator.hpp"      // 관찰 대상 자세(MoveNet) 판정
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

// MoveNet 모델 경로 (server/ 기준 상대경로). 라즈베리에 이 위치로 파일을
// 옮겨둘 것: server/models/movenet_lightning_int8.tflite (mkdir -p models)
const std::string kPoseModelPath = "models/movenet_lightning_int8.tflite";
// 관찰 대상 1명당 자세 확인 최대 주기 (2fps). movenet_bench.py 실측(RPi4,
// 평균 36ms/초당 28회) 기준 4채널×2명 정도까지 여유 있게 감당하는 값.
constexpr double kPoseIntervalSec = 0.5;

// 정규화 bbox(0~1) → 픽셀 cv::Rect. MoveNet은 사람 전신이 크롭 안에 다
// 들어와야 관절을 잘 잡으므로 bbox 폭/높이의 kCropMargin만큼 여유를 두고,
// 이미지 경계로 clamp한다.
cv::Rect normBoxToRect(float left, float top, float right, float bottom,
                       int imgW, int imgH) {
    constexpr float kCropMargin = 0.08f;
    const float w = right - left, h = bottom - top;
    left -= w * kCropMargin;
    right += w * kCropMargin;
    top -= h * kCropMargin;
    bottom += h * kCropMargin;

    const int x0 = std::max(0, static_cast<int>(left * imgW));
    const int y0 = std::max(0, static_cast<int>(top * imgH));
    const int x1 = std::min(imgW, static_cast<int>(right * imgW));
    const int y1 = std::min(imgH, static_cast<int>(bottom * imgH));
    return cv::Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

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

    // 채널별 침대 ROI (Qt에서 그려 보낸 정규화 0~1 다각형). 수신 스레드(콜백)와
    // 처리 스레드가 공유하므로 뮤텍스로 보호. 낙상 룰엔진의 침상 재실/이탈 판정
    // 및 되돌려보내는 영상 오버레이에 쓰인다.
    std::mutex roi_mutex;
    std::map<int, std::vector<std::pair<float, float>>> channel_rois;
    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        std::lock_guard<std::mutex> lock(roi_mutex);
        if (up.clear) {
            channel_rois.erase(up.channel);
            std::fprintf(stderr, "[roi] ch%d 침대 ROI 삭제됨\n", up.channel);
        } else {
            channel_rois[up.channel] = up.points;
            std::fprintf(stderr, "[roi] ch%d 침대 ROI 설정됨 (%zu점)\n",
                         up.channel, up.points.size());
        }
    });

    if (!stream_server.start()) {  // 콜백 등록 후 수신 시작
        return 1;
    }


    Database db;
    if (!db.connect("127.0.0.1", "daboijo", "1234", "daboijo")) {
        std::fprintf(stderr, "경고: DB 연결 실패 — 케어로그 저장 안 됨\n");
    }

    // 보호사 색(HSV) 감지기 + 채널별 케어시간 타이머.
    // 매 프레임 보호사 존재 여부를 판정해 케어 세션(재실 시간)을 집계한다.
    CaregiverDetector caregiver_detector;
    std::map<int, CareTimer> care_timers;

    // 채널별 최신 감지 결과 저장소 (메타데이터 콜백 스레드 ↔ 메인 스레드 공유).
    // 교차 검증 룰엔진(core)이 이후 여기서 사람 위치를 읽어간다.
    std::mutex det_mutex;
    std::map<int, std::vector<Detection>> latest_detections;
    std::map<int, uint64_t> det_updates;  // 채널별 갱신 횟수 (리포트용)

    // 1차 낙상 판정기 (임계값은 잠정값 — fall_detector.cpp 상단 주석 참조)
    // TODO(core): 웨어러블 바이탈과 교차 검증해 최종 판정으로 승격




    FallDetector fall_detector;
    fall_detector.setFallCallback([](int ch, const Detection& at) {
        std::fprintf(stderr,
                      "🚨 [ch%d] 낙상 의심! 위치=(%.2f,%.2f) — MoveNet 자세 판정 기준\n",
                      ch, at.cx, at.cy);
    });

    // 관찰 대상(침대 밖 사람) 자세 확인용 MoveNet. 모델 파일이 없거나 로드에
    // 실패해도 크래시 없이 isReady()=false로 비활성화되고, 그 경우
    // isLyingDown()은 항상 false를 반환한다(=자세 기반 낙상 판정만 꺼짐,
    // 나머지 파이프라인은 정상 동작).
    PoseEstimator pose_estimator(kPoseModelPath);
    if (!pose_estimator.isReady()) {
        std::fprintf(stderr,
                     "경고: MoveNet 모델 로드 실패(%s) — 자세 기반 낙상 판정 비활성\n",
                     kPoseModelPath.c_str());
    }
    // 채널 → (ObjectId → 마지막 자세 확인 시각). kPoseIntervalSec 간격 제한용.
    std::map<int, std::map<int, std::chrono::steady_clock::time_point>> last_pose_time;

    FrameQueue queue(16);
    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        auto client = std::make_unique<RtspAvClient>(cam.channel, cam.url, queue);
        client->setDetectionCallback(
            [&](int ch, std::vector<Detection> dets) {
                // 이 채널의 침대 ROI 사본을 뜬다 — 낙상 판정기의 침상 게이팅 입력.
                // (재실/이탈 로그와 판정은 이제 fall_detector가 담당)
                std::vector<std::pair<float, float>> roi;
                {
                    std::lock_guard<std::mutex> lock(roi_mutex);
                    auto it = channel_rois.find(ch);
                    if (it != channel_rois.end()) roi = it->second;
                }
                std::lock_guard<std::mutex> lock(det_mutex);
                fall_detector.update(ch, dets, roi);
                latest_detections[ch] = std::move(dets);
                det_updates[ch] += 1;
            });
        client->start();
        clients.push_back(std::move(client));

        // [추가] 이 채널의 케어시간 타이머 생성 + 세션 종료 콜백 등록
        care_timers.emplace(cam.channel, CareTimer(3.0, 5.0));
        care_timers[cam.channel].onSessionEnd([ch = cam.channel, &db](int dur) {
            std::printf("[ch%d] 케어 세션 종료: %d초\n", ch, dur);
            db.insertCareLog(ch, dur);
        });
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

            // [추가] --- 보호사 인식 + 케어시간 측정 ---
            // 최신 감지 결과를 락 짧게 잡고 복사만 (무거운 색분석은 락 밖에서)
            std::vector<Detection> dets_copy;
            {
                std::lock_guard<std::mutex> lock(det_mutex);
                dets_copy = latest_detections[frame->channel];
            }
            DetectionFrame df;
            df.channel = frame->channel;
            df.objects = std::move(dets_copy);

            bool present = caregiver_detector.detectInFrame(frame->image, df);
            care_timers[frame->channel].update(present);

            // [추가] --- 관찰 대상 자세 확인 (MoveNet) ---
            // 침대 밖(관찰모드)인 사람만, 채널·사람당 kPoseIntervalSec 간격으로
            // bbox를 크롭해 자세를 확인한다. 평상시(다들 침대 안)엔 추론이
            // 전혀 안 돌아 CPU를 영상 파이프라인에 그대로 양보한다.
            if (pose_estimator.isReady()) {
                auto observed = fall_detector.observedTracks(frame->channel);
                auto pose_now = std::chrono::steady_clock::now();
                auto& channel_last = last_pose_time[frame->channel];
                for (const auto& t : observed) {
                    auto& last = channel_last[t.object_id];
                    if (std::chrono::duration<double>(pose_now - last).count() <
                        kPoseIntervalSec) {
                        continue;
                    }
                    last = pose_now;

                    cv::Rect roi = normBoxToRect(t.left, t.top, t.right, t.bottom,
                                                 frame->image.cols, frame->image.rows);
                    if (roi.width <= 0 || roi.height <= 0) continue;

                    bool lying = pose_estimator.isLyingDown(frame->image(roi));
                    fall_detector.reportPose(frame->channel, t.object_id, lying);
                }
            }
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