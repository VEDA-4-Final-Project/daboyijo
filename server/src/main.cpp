// 다보이조 중앙 서버 진입점.
// 파이프라인: RTSP 4채널 수신(libav) → [영상] 리사이즈→JPEG→Qt 송출
//                                    → [메타] WiseAI 객체감지 XML 파싱→감지 저장
// 5초마다 채널별 fps·사람 수·CPU·온도를 출력한다.

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
#include <thread>
#include <chrono>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "caregiver_detector.hpp"
#include "care_timer.hpp"
#include "database.hpp"
#include "detection.hpp"
#include "fall_detector.hpp"
#include "frame_queue.hpp"
#include "pose_estimator.hpp"
#include "protocol/video_stream.h"
#include "rtsp_av_client.hpp"
#include "stream_server.hpp"
#include "system_stats.hpp"
#include "privacy_masker.hpp"

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

// Thunder(256x256)로 교체 — Lightning보다 느리지만(실측 100.8ms vs 36ms) 정확도가
// 높음. 우리 실부하(채널당 1~2명, 2초 간격)는 초당 2~4회 추론이면 되고 Thunder
// 예산(초당 9.9회, movenet_bench.py 실측)에 여유가 있어 시도해볼 만하다.
const std::string kPoseModelPath = "models/movenet_thunder_int8.tflite";
constexpr double kPoseIntervalSec = 2.0;
// Thunder는 추론 1회당 4코어를 ~100ms 점유한다(Lightning 36ms의 ~3배) — 기본
// 스레드 수(4) 그대로면 그 순간 RTSP 디코딩·인코딩과 코어를 다퉈 fps가 흔들릴
// 수 있어, 영상 파이프라인에 코어를 남기도록 절반(2)으로 제한한다.
constexpr int kPoseNumThreads = 2;

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

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(ws) - begin + 1);
}

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
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        if (key == "stream_port") {
            config.stream_port = std::stoi(value);
            continue;
        }
        if (key.empty() ||
            !std::all_of(key.begin(), key.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }
        config.cameras.push_back({std::stoi(key), std::move(value)});
    }
    return config;
}

// 🌟 [추가] 메인 스레드와 AI 스레드 간 일감을 전달할 데이터 바구니 구조체
struct AiJob {
    cv::Mat raw_frame;           // MoveNet용 원본 이미지
    cv::Mat small_frame;         // 보호사 색상 감지용 축소 이미지
    int channel = -1;
    std::vector<Detection> dets; // 객체 좌표 메타데이터
};

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/cameras.conf";
    auto config = loadConfig(config_path);
    if (config.cameras.empty()) return 1;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    StreamServer stream_server(config.stream_port);
    std::mutex roi_mutex;
    std::map<int, std::vector<std::pair<float, float>>> channel_rois;
    
    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        std::lock_guard<std::mutex> lock(roi_mutex);
        if (up.clear) channel_rois.erase(up.channel);
        else channel_rois[up.channel] = up.points;
    });

    if (!stream_server.start()) return 1;

    Database db;
    db.connect("127.0.0.1", "daboijo", "1234", "daboijo");

    CaregiverDetector caregiver_detector;

    PrivacyMasker privacy_masker(10.0, 31);

    std::mutex det_mutex;
    std::map<int, std::vector<Detection>> latest_detections;
    std::map<int, uint64_t> det_updates;  // 채널별 갱신 횟수 (리포트용)

    // 1차 낙상 판정기 (임계값은 잠정값 — fall_detector.cpp 상단 주석 참조)
    // TODO(core): 웨어러블 바이탈과 교차 검증해 최종 판정으로 승격
    std::map<int, CareTimer> care_timers;

    FallDetector fall_detector;
    // 🌟 [수정] 람다 캡처에 [&]를 사용하여 privacy_masker 참조 전달
    fall_detector.setFallCallback([&](int ch, const Detection& at) {
        std::fprintf(stderr, "🚨 [ch%d] 낙상 의심! (자세 판정) cx=%.2f cy=%.2f\n", ch, at.cx, at.cy);
        // 🌟 [추가] 낙상 트리거 발생 시 마스킹 즉시 해제
        privacy_masker.reportFall(ch);
        // Qt 관제 화면에 통보 → 채널 강조 + 팝업 (protocol/video_stream.h 이벤트)
        stream_server.broadcastEvent(ch, DBJ_EVT_FALL, at.cx, at.cy);
    });

    PoseEstimator pose_estimator(kPoseModelPath, kPoseNumThreads);
    if (!pose_estimator.isReady()) {
        std::fprintf(stderr, "경고: MoveNet 로드 실패, 자세 판정 꺼짐\n");
    }

    FrameQueue queue(16);
    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        auto client = std::make_unique<RtspAvClient>(cam.channel, cam.url, queue);
        client->setDetectionCallback([&](int ch, std::vector<Detection> dets) {
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

        care_timers.emplace(cam.channel, CareTimer(3.0, 5.0));
        care_timers[cam.channel].onSessionEnd([ch = cam.channel, &db](int dur) {
            std::printf("[ch%d] 케어 세션 종료: %d초\n", ch, dur);
            db.insertCareLog(ch, dur);
        });
    }

    const cv::Size kViewSize(960, 540);
    const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};

    struct ChannelStats { uint64_t processed = 0; uint64_t bytes = 0; };
    std::map<int, ChannelStats> stats;
    SystemStats system_stats;
    double encode_ms_total = 0;
    uint64_t encode_count = 0;
    auto last_report = std::chrono::steady_clock::now();
    std::vector<uint64_t> last_counts(clients.size(), 0);

    // ====================================================================
    // 🕵️‍♂️ [2번 요리사] 무거운 AI 연산 전담 스레드 생성 (멀티스레딩)
    // ====================================================================
    std::mutex ai_mutex;
    std::map<int, AiJob> pending_ai_jobs; // 채널별 최신 일감 1장씩만 보관

    std::thread ai_worker([&]() {
        std::map<int, std::map<int, std::chrono::steady_clock::time_point>> last_pose_time;

        while (!g_stop) {
            AiJob job;
            bool got_job = false;

            // 1. 일감 바구니에 새 영상이 있는지 확인하고 꺼내오기
            {
                std::lock_guard<std::mutex> lock(ai_mutex);
                if (!pending_ai_jobs.empty()) {
                    auto it = pending_ai_jobs.begin();
                    job = std::move(it->second);
                    pending_ai_jobs.erase(it); // 꺼낸 일감은 삭제
                    got_job = true;
                }
            }

            // 일감이 없으면 CPU를 쉬게 해줍니다 (10ms 대기)
            if (!got_job) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 2. 가벼워진 small 이미지로 보호사 인식 수행
            DetectionFrame df;
            df.channel = job.channel;
            df.objects = job.dets;
            bool present = caregiver_detector.detectInFrame(job.small_frame, df);
            care_timers[job.channel].update(present);

            // 3. 무거운 원본 이미지로 MoveNet 자세 판정 수행
            if (pose_estimator.isReady()) {
                std::vector<FallDetector::ObservedTrack> observed;
                {
                    // fall_detector 내부 자원 보호를 위해 락 사용
                    std::lock_guard<std::mutex> lock(det_mutex);
                    observed = fall_detector.observedTracks(job.channel);
                }

                auto pose_now = std::chrono::steady_clock::now();
                auto& channel_last = last_pose_time[job.channel];

                for (const auto& t : observed) {
                    auto& last = channel_last[t.object_id];
                    if (std::chrono::duration<double>(pose_now - last).count() < kPoseIntervalSec) {
                        continue; // 설정된 2초 대기 시간 안 지났으면 스킵
                    }
                    last = pose_now;

                    cv::Rect roi = normBoxToRect(t.left, t.top, t.right, t.bottom,
                                                 job.raw_frame.cols, job.raw_frame.rows);
                    if (roi.width <= 0 || roi.height <= 0) {
                        std::fprintf(stderr,
                                     "[pose] ch%d obj%d 크롭 실패 (bbox=%.2f,%.2f,%.2f,%.2f)\n",
                                     job.channel, t.object_id, t.left, t.top, t.right, t.bottom);
                        continue;
                    }

                    // 🚨 AI 스레드가 혼자서 무거운 연산을 처리 (메인 스트리밍은 방해받지 않음!)
                    bool lying = pose_estimator.isLyingDown(job.raw_frame(roi));
                    std::fprintf(stderr, "[pose] ch%d obj%d 판정=%s (crop %dx%d)\n",
                                 job.channel, t.object_id, lying ? "누움" : "서있음",
                                 roi.width, roi.height);

                    {
                        std::lock_guard<std::mutex> lock(det_mutex);
                        fall_detector.reportPose(job.channel, t.object_id, lying);
                    }
                }
            }
        }
    });
    // ====================================================================

    // 프레임 레이트 방어선 (메인 루프 폭주 방지)
    std::map<int, std::chrono::steady_clock::time_point> last_main_proc_time;
    const double kMainProcessInterval = 0.1; // 최대 10fps 제한 (0.1초)

    // 👨‍🍳 [1번 요리사] 영상 스트리밍 전담 메인 스레드 (초고속 동작)
    while (!g_stop) {
        auto frame = queue.pop(std::chrono::milliseconds(200));
        if (frame) {
            auto now = std::chrono::steady_clock::now();
            
            // 너무 빨리 들어온 프레임은 버려서 라즈베리 파이 발열 방지
            if (std::chrono::duration<double>(now - last_main_proc_time[frame->channel]).count() < kMainProcessInterval) {
                continue; 
            }
            last_main_proc_time[frame->channel] = now;

            auto t0 = std::chrono::steady_clock::now();

            // 1. 빠른 리사이즈
            cv::Mat small;
            cv::resize(frame->image, small, kViewSize);

            // 2. 최신 메타데이터 복사
            std::vector<Detection> dets_copy;
            {
                std::lock_guard<std::mutex> lock(det_mutex);
                dets_copy = latest_detections[frame->channel];
            }

            // 3. AI 워커 스레드에게 최신 일감 1장 던져주기 (덮어쓰기 방식으로 밀림 방지)
            {
                std::lock_guard<std::mutex> lock(ai_mutex);
                pending_ai_jobs[frame->channel] = {
                    frame->image.clone(),  // AI MoveNet을 위한 원본 백업
                    small.clone(),         // AI 보호사 인식을 위한 소형본 백업
                    frame->channel,
                    std::move(dets_copy)
                };
            }

            // 4. 다이나믹 프라이버시 마스크 적용 (GUI 송출용 small 이미지에만 덧씌움)
            privacy_masker.process(frame->channel, small, dets_copy);

            // 5. 마스킹이 완료된 이미지를 인코딩해서 Qt로 송출
            std::vector<unsigned char> jpeg;
            cv::imencode(".jpg", small, jpeg, kJpegParams);

            auto& ch = stats[frame->channel];
            ch.processed += 1;
            ch.bytes += jpeg.size();
            stream_server.broadcast(frame->channel, std::move(jpeg));

            encode_ms_total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            encode_count += 1;
        }

        // --- 5초마다 상태 출력 로그 ---
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report);
        if (elapsed.count() >= 5) {
            std::ostringstream status;
            for (size_t i = 0; i < clients.size(); ++i) {
                int id = clients[i]->channel();
                uint64_t count = clients[i]->frameCount();
                double in_fps = static_cast<double>(count - last_counts[i]) / elapsed.count();
                double out_fps = static_cast<double>(stats[id].processed) / elapsed.count();

                int humans = 0;
                {
                    std::lock_guard<std::mutex> lock(det_mutex);
                    for (const auto& d : latest_detections[id]) {
                        if (d.isHuman()) ++humans;
                    }
                }
                char buf[112];
                std::snprintf(buf, sizeof(buf), "[ch%d] %s in %.1f out %.1ffps 사람%d  ",
                              id, clients[i]->connected() ? "OK" : "끊김", in_fps, out_fps, humans);
                status << buf;
                last_counts[i] = count;
                stats[id] = ChannelStats{};
            }

            double avg_encode = encode_count ? encode_ms_total / encode_count : 0;
            char sys_buf[96];
            std::snprintf(sys_buf, sizeof(sys_buf), "| CPU %.0f%% %.1f°C 인코딩 %.1fms 클라 %zu",
                          system_stats.cpuPercent(), SystemStats::socTemperature(), avg_encode, stream_server.clientCount());
            status << sys_buf;
            
            encode_ms_total = 0;
            encode_count = 0;
            std::printf("%s\n", status.str().c_str());
            last_report = now;
        }
    }

    std::printf("종료 중...\n");
    g_stop = 1;

    // 🌟 [추가] 메인 함수가 죽기 전에 AI 스레드가 퇴근할 때까지 기다려줌
    if (ai_worker.joinable()) {
        ai_worker.join();
    }

    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    return 0;
}