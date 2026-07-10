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
#include "detection.hpp"
#include "fall_detector.hpp"
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

// 침대 ROI 다각형(정규화 0~1)을 프레임에 그린다 — Qt가 보낸 영역이 서버에
// 제대로 도착했는지 되돌아오는 영상에서 바로 확인하기 위함(라운드트립 검증).
void drawRoi(cv::Mat& frame, const std::vector<std::pair<float, float>>& roi) {
    if (roi.size() < 2) return;
    const int w = frame.cols;
    const int h = frame.rows;
    std::vector<cv::Point> pts;
    pts.reserve(roi.size());
    for (const auto& p : roi) {
        pts.emplace_back(static_cast<int>(p.first * w),
                         static_cast<int>(p.second * h));
    }
    const cv::Scalar kBed(255, 180, 40);  // 하늘색 계열 (BGR) — 침대 영역
    // 반투명 채움
    cv::Mat overlay = frame.clone();
    std::vector<std::vector<cv::Point>> polys{pts};
    cv::fillPoly(overlay, polys, kBed);
    cv::addWeighted(overlay, 0.25, frame, 0.75, 0, frame);
    cv::polylines(frame, polys, true, kBed, 2, cv::LINE_AA);
    cv::putText(frame, "BED ROI", pts[0] + cv::Point(4, -6),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, kBed, 1, cv::LINE_AA);
}

// 점 (px,py)가 정규화 다각형 roi 안에 있는지 (ray-casting). 침상 재실/이탈 판정용.
bool pointInRoi(float px, float py,
                const std::vector<std::pair<float, float>>& roi) {
    bool inside = false;
    for (size_t i = 0, j = roi.size() - 1; i < roi.size(); j = i++) {
        float xi = roi[i].first, yi = roi[i].second;
        float xj = roi[j].first, yj = roi[j].second;
        bool crosses = ((yi > py) != (yj > py)) &&
                       (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
        if (crosses) inside = !inside;
    }
    return inside;
}

// 감지 객체의 bbox·무게중심을 프레임에 그려 넣는다 (좌표 정합성 확인용).
// Detection 좌표는 0~1 정규화 → 프레임 픽셀로 환산. 이 박스가 화면 속 사람
// 위에 정확히 얹히면 메타데이터↔영상 좌표계가 일치한다는 뜻 → 방법 B 성립.
// 요약 프레임(넓이 0, 객체 소실 시 오는 것)은 그리지 않는다.
void drawDetections(cv::Mat& frame, const std::vector<Detection>& dets) {
    const int w = frame.cols;
    const int h = frame.rows;
    for (const auto& d : dets) {
        if (d.width() <= 0 || d.height() <= 0) continue;

        cv::Point p1(static_cast<int>(d.left * w), static_cast<int>(d.top * h));
        cv::Point p2(static_cast<int>(d.right * w), static_cast<int>(d.bottom * h));

        // Human=초록, Head=노랑, 그 외=회색 (BGR)
        cv::Scalar color = d.isHuman()      ? cv::Scalar(0, 255, 0)
                           : d.type == "Head" ? cv::Scalar(0, 255, 255)
                                              : cv::Scalar(160, 160, 160);
        cv::rectangle(frame, p1, p2, color, 2);

        // 무게중심(cx,cy) — 낙상 판정이 실제로 쓰는 점. 박스와 함께 보면
        // cy 좌표가 사람 몸통 중앙에 찍히는지도 같이 확인된다.
        cv::circle(frame,
                   cv::Point(static_cast<int>(d.cx * w),
                             static_cast<int>(d.cy * h)),
                   3, color, -1);

        // 라벨: 타입#ID 확률 — 박스 위(공간 없으면 아래)
        char label[48];
        std::snprintf(label, sizeof(label), "%s#%d %.2f", d.type.c_str(),
                      d.object_id, d.likelihood);
        int baseline = 0;
        cv::Size ts = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1,
                                      &baseline);
        int ty = (p1.y - 4 < ts.height) ? p1.y + ts.height + 4 : p1.y - 4;
        cv::putText(frame, label, cv::Point(p1.x, ty), cv::FONT_HERSHEY_SIMPLEX,
                    0.4, color, 1, cv::LINE_AA);
    }
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
                      "🚨 [ch%d] 낙상 의심! 위치=(%.2f,%.2f) — 잠정 임계값 기준, 캡처로 검증 필요\n",
                      ch, at.cx, at.cy);
    });

    // 채널별 침상 재실 상태 (ROI 안에 사람이 있었는지). roi_mutex로 보호.
    std::map<int, bool> bed_occupied;

    FrameQueue queue(16);
    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        auto client = std::make_unique<RtspAvClient>(cam.channel, cam.url, queue);
        client->setDetectionCallback(
            [&](int ch, std::vector<Detection> dets) {
                // 침상 재실/이탈 판정 (ROI 설정된 채널만). 이후 낙상 룰엔진이
                // "이탈 직후 바닥에 누움"을 낙상 신호로 승격시킬 지점이다.
                {
                    std::lock_guard<std::mutex> lock(roi_mutex);
                    auto it = channel_rois.find(ch);
                    if (it != channel_rois.end() && it->second.size() >= 3) {
                        bool in_bed = false;
                        for (const auto& d : dets) {
                            if (d.isHuman() && d.width() > 0 && d.height() > 0 &&
                                pointInRoi(d.cx, d.cy, it->second)) {
                                in_bed = true;
                                break;
                            }
                        }
                        bool& was = bed_occupied[ch];
                        if (was && !in_bed) {
                            std::fprintf(stderr, "[roi] ch%d 침상 이탈\n", ch);
                        } else if (!was && in_bed) {
                            std::fprintf(stderr, "[roi] ch%d 침상 재실\n", ch);
                        }
                        was = in_bed;
                    }
                }
                std::lock_guard<std::mutex> lock(det_mutex);
                fall_detector.update(ch, dets);
                latest_detections[ch] = std::move(dets);
                det_updates[ch] += 1;
            });
        client->start();
        clients.push_back(std::move(client));

        // [추가] 이 채널의 케어시간 타이머 생성 + 세션 종료 콜백 등록
        care_timers.emplace(cam.channel, CareTimer(3.0, 5.0));
        care_timers[cam.channel].onSessionEnd([ch = cam.channel](int dur) {
            std::printf("[ch%d] 케어 세션 종료: %d초 (DB 연동 예정)\n", ch, dur);
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

            // 침대 ROI를 먼저 깔고(반투명), 그 위에 감지 박스를 그린다.
            // ROI는 Qt가 보낸 게 서버에 도착했는지 되돌아오는 영상으로 검증하는 용도.
            {
                std::lock_guard<std::mutex> lock(roi_mutex);
                auto it = channel_rois.find(frame->channel);
                if (it != channel_rois.end()) drawRoi(small, it->second);
            }
            // 좌표 정합성 확인용: 이 채널 최신 감지 박스를 영상에 구워 넣는다.
            // 프로토콜·Qt 변경 없이 박스가 사람 위에 맞는지 눈으로 검증한다.
            // TODO(video): 검증 끝나면 설정 플래그로 토글 (기본 off).
            {
                std::lock_guard<std::mutex> lock(det_mutex);
                drawDetections(small, latest_detections[frame->channel]);
            }

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