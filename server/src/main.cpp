// 다보이조 중앙 서버 진입점.
// 파이프라인: RTSP 4채널 수신(libav, 960x540 BGR로 다운스케일 완료) → [영상] JPEG→Qt 송출
//                                    → [메타] WiseAI 객체감지 XML 파싱→감지 저장
// 5초마다 채널별 fps·사람 수·CPU·온도를 출력한다.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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

#include "blackbox_recorder.hpp"
#include "caregiver_detector.hpp"
#include "care_timer.hpp"
#include "clip_http_server.hpp"
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

// ── 블랙박스(낙상 전후 영상 저장) ────────────────────────────────
// 채널별로 최근 kBlackboxPreSec초 압축 영상을 버퍼링하다가, 낙상 등 이벤트
// 발생 시 [이벤트 전 preSec + 이벤트 후 postSec] 구간을 mp4로 저장한다.
// 디코딩 전 압축 패킷을 그대로 버퍼링/remux하므로 채널당 버퍼는 수 MB
// 수준(720p 수 Mbps 스트림 기준)이라 라즈베리파이에서도 부담이 거의 없다.
const std::string kBlackboxDir = "blackbox_clips";
constexpr double kBlackboxPreSec = 5.0;
constexpr double kBlackboxPostSec = 5.0;
// 저장된 클립을 Qt가 QMediaPlayer로 바로 재생할 수 있게 서빙하는 HTTP 포트.
constexpr int kClipHttpPort = 5501;

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
    // 마스킹 전 깨끗한 960x540 프레임 — MoveNet 크롭과 보호사 색상 감지 공용.
    // (수신단 sws 다운스케일로 원본 해상도 프레임은 더 이상 파이프라인에 없음.
    //  Thunder 입력이 256x256이라 960x540 크롭으로도 충분 — [pose] 로그로 검증)
    cv::Mat frame;
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
    PrivacyMasker privacy_masker;

    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        std::lock_guard<std::mutex> lock(roi_mutex);
        if (up.clear) channel_rois.erase(up.channel);
        else channel_rois[up.channel] = up.points;
    });
    
    stream_server.setConfirmCallback([&](int ch) {
        privacy_masker.clearFall(ch);
        std::printf("ch%d 낙상 경보 확인.\n", ch);
    });

    if (!stream_server.start()) return 1;

    std::filesystem::create_directories(kBlackboxDir);
    ClipHttpServer clip_http(kClipHttpPort, kBlackboxDir);
    if (!clip_http.start()) {
        std::fprintf(stderr, "경고: 블랙박스 클립 HTTP 서버 시작 실패 (포트 %d) — 블랙박스 저장은 계속되지만 Qt에서 재생은 안 됨\n",
                     kClipHttpPort);
    }

    Database db;
    db.connect("127.0.0.1", "daboijo", "1234", "daboijo");

    CaregiverDetector caregiver_detector;

    std::mutex det_mutex;
    struct TimestampedDets {
    std::chrono::steady_clock::time_point timestamp;
    std::vector<Detection> detections;
};
std::map<int, std::vector<TimestampedDets>> detection_history;
    std::map<int, uint64_t> det_updates;  // 채널별 갱신 횟수 (리포트용)

    // 1차 낙상 판정기 (임계값은 잠정값 — fall_detector.cpp 상단 주석 참조)
    // TODO(core): 웨어러블 바이탈과 교차 검증해 최종 판정으로 승격
    std::map<int, CareTimer> care_timers;

    // 채널별 블랙박스 레코더 — 아래 카메라 루프에서 채워진다.
    std::map<int, std::unique_ptr<BlackboxRecorder>> blackbox_recorders;

    FallDetector fall_detector;
    // 🌟 [수정] 람다 캡처에 [&]를 사용하여 privacy_masker 참조 전달
    fall_detector.setFallCallback([&](int ch, const Detection& at) {
        std::fprintf(stderr, "🚨 [ch%d] 낙상 의심! (자세 판정) cx=%.2f cy=%.2f\n", ch, at.cx, at.cy);
        // 낙상 트리거 발생 시 마스킹 즉시 해제
        privacy_masker.reportFall(ch);

        // 블랙박스: 지금까지 버퍼(최근 kBlackboxPreSec초) + 이후 kBlackboxPostSec초
        // 저장 예약. 반환된 시각을 그대로 이벤트 메시지에 실어 보내면, Qt가
        // 클립 파일명(ch{채널}_{시각}.mp4)을 그대로 재구성해 재생 요청할 수 있다.
        int64_t evt_ms = 0;
        auto rec_it = blackbox_recorders.find(ch);
        if (rec_it != blackbox_recorders.end()) {
            evt_ms = rec_it->second->trigger();
        }

        // Qt 관제 화면에 통보 → 채널 강조 + 팝업 (protocol/video_stream.h 이벤트)
        stream_server.broadcastEvent(ch, DBJ_EVT_FALL, at.cx, at.cy, evt_ms);
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
            auto now = std::chrono::steady_clock::now();
            detection_history[ch].push_back({now, std::move(dets)});

            // 메모리가 꽉 차는 걸 방지하기 위해 최근 30개 기록만 남기고 옛날 것은 자동 삭제
            if (detection_history[ch].size() > 30) {
                detection_history[ch].erase(detection_history[ch].begin());
            }
            det_updates[ch] += 1;
        });
        auto recorder = std::make_unique<BlackboxRecorder>(
            cam.channel, kBlackboxDir, kBlackboxPreSec, kBlackboxPostSec);
        recorder->onClipReady([](int rch, const std::string& path, int64_t eventMs) {
            std::printf("[ch%d] 블랙박스 저장 완료: %s (이벤트 %lld)\n", rch, path.c_str(),
                        static_cast<long long>(eventMs));
        });
        BlackboxRecorder* recorder_ptr = recorder.get();
        client->setStreamReadyCallback(
            [recorder_ptr](const AVCodecParameters* cp, int tbn, int tbd) {
                recorder_ptr->setStreamInfo(cp, tbn, tbd);
            });
        client->setPacketCallback([recorder_ptr](const AVPacket* pkt) {
            recorder_ptr->onPacket(pkt);
        });
        blackbox_recorders[cam.channel] = std::move(recorder);

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

            // 2. 보호사 인식 수행 (깨끗한 프레임 사용)
            DetectionFrame df;
            df.channel = job.channel;
            df.objects = job.dets;
            bool present = caregiver_detector.detectInFrame(job.frame, df);
            care_timers[job.channel].update(present);

            // 3. MoveNet 자세 판정 수행 (같은 프레임에서 사람 bbox 크롭)
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
                                                 job.frame.cols, job.frame.rows);
                    if (roi.width <= 0 || roi.height <= 0) {
                        // 로그 정리로 비활성화 — 디버깅 시 해제
                        // std::fprintf(stderr,
                        //              "[pose] ch%d obj%d 크롭 실패 (bbox=%.2f,%.2f,%.2f,%.2f)\n",
                        //              job.channel, t.object_id, t.left, t.top, t.right, t.bottom);
                        continue;
                    }

                    // 🚨 AI 스레드가 혼자서 무거운 연산을 처리 (메인 스트리밍은 방해받지 않음!)
                    bool lying = pose_estimator.isLyingDown(job.frame(roi));
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
    const double kMainProcessInterval = 1.0 / 15.0; // 최대 15fps 제한
    // (rtsp_av_client.cpp의 kMaxConvertFps가 이보다 커야 여기까지 프레임이 온다)

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

            // 1. 수신단(sws 다운스케일)에서 이미 960x540 BGR로 도착.
            //    다른 크기가 오면(설정 변경 등 방어) 여기서 맞춘다.
            cv::Mat small = frame->image;
            if (small.size() != kViewSize) cv::resize(small, small, kViewSize);
            // AI 전송용 깨끗한 복사본 (아래 마스킹이 small을 제자리 수정하므로 필수)
            cv::Mat clean = small.clone();

            // 2. 시간 오차가 적은 과거의 좌표를 탐색
            std::vector<Detection> dets_copy;
            {
                std::lock_guard<std::mutex> lock(det_mutex);
                const auto& history = detection_history[frame->channel];

                if (!history.empty()) {
                    auto best_it = history.begin();
                    double min_diff = std::numeric_limits<double>::max();

                    for (auto it = history.begin(); it != history.end(); ++it) {
                        // 영상의 생성 시간과 좌표의 생성 시간 차이를 계산 (초 단위)
                        double diff = std::abs(std::chrono::duration<double>(it->timestamp - frame->timestamp).count());
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_it = it;
                        }
                    }
                    dets_copy = best_it->detections; // 가장 궁합이 잘 맞는 시간대의 좌표 선택!
                }
            }

            // 3. 다이나믹 프라이버시 마스크 적용 (GUI 송출용 small 이미지에만 덧씌움)
            privacy_masker.process(frame->channel, small, dets_copy);

            // 4. AI 워커 스레드에게 최신 일감 1장 던져주기 (덮어쓰기 방식으로 밀림 방지)
            {
                std::lock_guard<std::mutex> lock(ai_mutex);
                pending_ai_jobs[frame->channel] = {
                    std::move(clean),
                    frame->channel,
                    std::move(dets_copy)
                };
            }

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

    // 종료 시점에 이벤트가 걸린 채(post 구간 수집 중) 남아있던 블랙박스는
    // 못 채운 만큼이라도 저장해서 유실을 막는다.
    for (auto& [rch, recorder] : blackbox_recorders) {
        (void)rch;
        recorder->flush();
    }

    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    clip_http.stop();
    return 0;
}