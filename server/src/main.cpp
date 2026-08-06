// 다보이조 중앙 서버 진입점 — "조립도".
// 여기서는 모듈을 만들고 서로 연결(배선)만 한다. 로직은 각 모듈 파일에 있다:
//
//   공용 인프라  : modules/video_pipeline.*  modules/ai_worker.*
//                  modules/detection_store.*  modules/stats_reporter.*
//   [낙상감지]   : modules/fall_module.*  (core/fall_detector.*, video/pose_estimator.*)
//   [블러처리]   : video/privacy_masker.*
//   [요양사감지] : modules/caregiver_module.*  (video/caregiver_detector.*, video/care_timer.*)
//   [블랙박스]   : modules/blackbox_module.*  (video/blackbox_recorder.*, core/clip_http_server.*)
//
// ★ 충돌 방지 규칙: 기능 수정은 자기 모듈 파일에서만. main.cpp를 고칠 일은
//   ① 새 모듈 추가 ② 모듈 간 연결 변경 두 가지뿐이며, 그때도 자기 기능의
//   배선 블록(주석으로 구분)만 건드린다.

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "ai_worker.hpp"
#include "blackbox_module.hpp"
#include "caregiver_module.hpp"
#include "config.hpp"
#include "database.hpp"
#include "detection_store.hpp"
#include "fall_module.hpp"
#include "frame_queue.hpp"
#include "privacy_masker.hpp"
#include "protocol/video_stream.h"
#include "onvif_imaging.hpp"
#include "rtsp_av_client.hpp"
#include "stats_reporter.hpp"
#include "stream_server.hpp"
#include "video_pipeline.hpp"
#include "bed_egress_module.hpp"
#include "telegram_module.hpp"
#include "snapshot_buffer.hpp"
#include "gemini_client.hpp"
#include "care_qa.hpp"

namespace {

std::sig_atomic_t g_stop = 0;

void handleSignal(int) {
    g_stop = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/cameras.conf";
    auto config = loadServerConfig(config_path);
    // cameras.conf는 이 서버(Pi)가 담당할 채널을 선언한다. URL이 비어 있어도
    // ("0=" 처럼) 그 채널 슬롯을 만들고 대기하다가, Qt의 "카메라 연결"을 받고
    // RTSP를 연다. 담당 채널이 하나도 없으면 설정 오류.
    if (config.cameras.empty()) {
        std::fprintf(stderr, "담당 채널이 없습니다 — cameras.conf에 채널을 선언하세요 (예: 0=)\n");
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // [보호자 알림] curl_easy_init()의 암묵적 초기화는 스레드 안전하지 않으므로,
    // 알림 스레드가 생기기 전에 여기서 한 번만 명시적으로 초기화한다.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ── 공용 인프라 ──────────────────────────────────────────────
    StreamServer stream_server(config.stream_port);
    // 채널별 전용 프레임 큐 — 한 채널이 몰아쳐도 다른 채널을 굶기지 않도록 격리.
    // 채널당 처리 스레드 1개가 자기 큐만 소비한다(VideoPipeline).
    std::map<int, std::unique_ptr<FrameQueue>> queues;
    // 채널별 RTSP 수신 워커. 카메라 미지정 채널은 URL 없이 만들어져 대기하고,
    // Qt의 "카메라 연결" 신호(stream_server 콜백)가 오면 그 채널만 reconnect한다.
    std::vector<std::unique_ptr<RtspAvClient>> clients;
    std::array<RtspAvClient*, 4> client_by_channel{};  // 채널→워커 (연결 신호 라우팅)
    // 채널별 최신 RTSP URL — ONVIF 이미지 조절 시 카메라 host/계정을 여기서 파싱.
    // 수신 스레드(CAMERA_SET)가 쓰고, 이미지 적용 스레드가 읽으므로 뮤텍스로 보호.
    std::array<std::string, 4> cam_url_by_channel{};
    std::mutex cam_url_mutex;
    DetectionStore detections;  // 감지 이력 저장 + 프레임-좌표 시간 매칭
    SnapshotBuffer snapshots;   // 버퍼 A: 전원 블러본 (Gemini/평상시 사진용)
    SnapshotBuffer snapshots_fall;  // 버퍼 B: 낙상 선택본 (낙상자만 노출, 보호자 사진용)
    AiWorker ai_worker;         // 무거운 AI 연산 전담 — 채널별 워커 스레드
    Database db;

    // ── 기능 모듈 생성 ───────────────────────────────────────────
    FallModule fall;                // [낙상감지]
    BedEgressModule bed_egress;     // [침상탈출]
    PrivacyMasker privacy_masker;   // [블러처리]
    CaregiverModule caregiver(db);  // [요양사감지]
    BlackboxModule blackbox;        // [블랙박스]
    TelegramModule telegram;        // [보호자 알림 + 케어봇]
    telegram.configure(config.telegram_bot_token, config.telegram_chat_id, config.telegram_chat_ids);

    // [케어봇] 버튼 메뉴 기반 상호작용: 보호자가 아무 메시지나 보내면 버튼 메뉴를
    // 띄우고(handleMessage), 버튼 클릭(handleCallback)으로 상황 조회·연락처·알림 토글.
    // ※ 낙상 확인(블러 원복)은 텔레그램에서 빼고 Qt 관제 화면(setConfirmCallback)만 담당.
    GeminiClient vlm(config.gemini_api_key, config.gemini_model);
    CareQaModule care_qa(snapshots, snapshots_fall, vlm, telegram);
    care_qa.setContacts(config.care_contact_caregiver, config.care_contact_manager);
    telegram.setCommandHandler([&](int ch, const std::string& chat_id,
                                   const std::string& text) {
        care_qa.handleMessage(ch, chat_id, text);
    });
    telegram.setCallbackHandler([&](int ch, const std::string& chat_id,
                                    const std::string& data) {
        care_qa.handleCallback(ch, chat_id, data);
    });

    // ── 모듈 간 배선 ─────────────────────────────────────────────
    // Qt가 그린 침대 ROI → 낙상 및 침상 탈출 판정기
    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        fall.updateBedRoi(up.channel, up.clear, up.points);
        bed_egress.updateBedRoi(up.channel, up.clear, up.points);
    });
    // Qt의 낙상 확인 신호 → 블러 원상복구
    stream_server.setConfirmCallback([&](int ch) {
        privacy_masker.clearFall(ch);
        std::printf("ch%d 낙상 경보 확인.\n", ch + 1);
    });
    //  Qt의 환자 정보 변경 신호 → 침상 탈출 모듈의 환자 관리 상태 갱신(인메모리).
    //  DB 영속화는 Qt가 residents.risk_level에 직접 기록하므로 서버는 하지 않는다
    //  (부팅 시 bed_egress.initializeFromDb → getRiskLevelByCamera로 복원).
    stream_server.setRiskLevelCallback([&](int ch, int patient_status) {
        bed_egress.updatePatientStatus(ch, patient_status);
    });
    // Qt의 "카메라 연결" 신호 → 해당 채널 RTSP 워커를 이 URL로 (재)연결.
    // (client_by_channel은 아래 채널 슬롯 구성 루프에서 채워지며, 이 콜백은 Qt가
    //  접속해 메시지를 보낼 때 비로소 호출된다.)
    stream_server.setCameraSetCallback([&](int ch, const std::string& url) {
        if (ch < 0 || ch >= 4 || !client_by_channel[ch]) return;
        std::printf("ch%d 카메라 연결 요청 수신 → RTSP 연결\n", ch + 1);
        {
            std::lock_guard<std::mutex> lk(cam_url_mutex);
            cam_url_by_channel[ch] = url;  // ONVIF 이미지 조절 시 재사용
        }
        client_by_channel[ch]->reconnect(url);
    });
    // Qt의 "카메라 해제" 신호 → 해당 채널 연결 종료 후 대기 상태로.
    stream_server.setCameraClearCallback([&](int ch) {
        if (ch < 0 || ch >= 4 || !client_by_channel[ch]) return;
        std::printf("ch%d 카메라 연결 해제\n", ch + 1);
        {
            std::lock_guard<std::mutex> lk(cam_url_mutex);
            cam_url_by_channel[ch].clear();
        }
        client_by_channel[ch]->disconnect();
    });
    // Qt의 "이미지 조절"(밝기/대비/채도) 신호 → 해당 채널 카메라에 ONVIF 적용.
    // ONVIF는 블로킹 HTTP라 수신 스레드를 막지 않도록 분리 스레드에서 처리한다.
    stream_server.setImageSetCallback(
        [&](int ch, int b, int c, int s) {
            if (ch < 0 || ch >= 4) return;
            std::string url;
            {
                std::lock_guard<std::mutex> lk(cam_url_mutex);
                url = cam_url_by_channel[ch];
            }
            if (url.empty()) {
                std::fprintf(stderr, "[image] ch%d 카메라 미연결 — 이미지 조절 무시\n", ch + 1);
                return;
            }
            OnvifImageParams p;
            p.brightness = b; p.contrast = c; p.saturation = s;
            std::thread([ch, url, p]() {
                std::string err;
                if (applyOnvifImaging(url, p, &err))
                    std::fprintf(stderr, "[image] ch%d 이미지 적용 성공\n", ch + 1);
                else
                    std::fprintf(stderr, "[image] ch%d 이미지 적용 실패: %s\n",
                                 ch + 1, err.c_str());
            }).detach();
        });
    // 낙상 확정 → 블러 즉시 해제 + 블랙박스 클립 저장 + Qt 경보
    fall.setFallCallback([&](int ch, const Detection& at) {
        std::fprintf(stderr, "🚨 [ch%d] 낙상 의심! (자세 판정) obj=%d cx=%.2f cy=%.2f\n",
                     ch + 1, at.object_id, at.cx, at.cy);
        privacy_masker.reportFall(ch, at.object_id, at.cx, at.cy);
        int64_t evt_ms = blackbox.trigger(ch, "FALL");
        stream_server.broadcastEvent(ch, DBJ_EVT_FALL, at.cx, at.cy, evt_ms);
        telegram.notifyFall(ch);      // 즉시 기본 알림
        care_qa.reportFall(ch);       // [케어봇] 몇 초 뒤 VLM 상황 설명+스냅샷 자동 전송
    });
    // 침상 탈출 -> 블랙박스 클립 저장 + Qt 경보
    bed_egress.setAlarmCallback([&](int ch, int obj_id) {
        std::fprintf(stderr, "⚠️ [ch%d] 환자 침상 탈출 감지! (obj: %d)\n", ch + 1, obj_id);
        int64_t evt_ms = blackbox.trigger(ch, "EGRESS");
        stream_server.broadcastEvent(ch, DBJ_EVT_EGRESS, 0.0f, 0.0f, evt_ms);
        telegram.notifyEgress(ch);
    });
    // AI 워커에 분석 프로세서 등록 (실행 순서 = 등록 순서)
    ai_worker.addProcessor([&](const AiJob& job) { caregiver.processFrame(job); });
    ai_worker.addProcessor([&](const AiJob& job) { fall.processFrame(job); });

    // ── 서버 기동 ────────────────────────────────────────────────
    if (!stream_server.start()) return 1;
    blackbox.startHttp();
    telegram.startPolling();  // [케어봇] getUpdates 롱폴링 스레드 기동
    db.connect(config.db_host, "daboijo", "1234", "daboijo");
    bed_egress.initializeFromDb(db);

    for (const auto& cam : config.cameras) {
        // 이 채널 전용 큐 생성 (용량 8 — 버스티 디코딩 출력을 흡수해 드랍 방지.
        // 소비자가 빠르면 큐는 거의 비어 있어 지연은 낮게 유지된다).
        auto& cam_queue =
            *(queues[cam.channel] = std::make_unique<FrameQueue>(8));
        // cam.url이 비어 있으면 워커는 연결하지 않고 대기 — Qt가 카메라를 지정하면
        // 위 setCameraSetCallback → reconnect(url)로 그 채널만 살아난다.
        auto client =
            std::make_unique<RtspAvClient>(cam.channel, cam.url, cam_queue);
        client->setDetectionCallback([&](int ch, std::vector<Detection> dets,
                                         std::chrono::steady_clock::time_point cap) {
            fall.onMetadata(ch, dets);             // 낙상: ROI 게이팅 + bbox 캐시
            bed_egress.processDetections(ch, dets);// 침상
            detections.push(ch, std::move(dets), cap);  // 공용: 시간 매칭용 이력 저장
        });
        blackbox.attachChannel(*client);    // 블랙박스: 압축 패킷 버퍼링 배선
        caregiver.addChannel(cam.channel);  // 요양사: 케어 타이머 준비
        fall.addChannel(cam.channel);       // 낙상: 채널 전용 MoveNet 로드
        ai_worker.addChannel(cam.channel);  // AI: 채널 전담 워커 스레드 예약
        if (cam.channel >= 0 && cam.channel < 4) {
            client_by_channel[cam.channel] = client.get();  // 연결 신호 라우팅용
        }
        client->start();
        clients.push_back(std::move(client));
    }
    std::printf("%zu개 채널 슬롯 구성 (Ctrl+C로 종료)\n", clients.size());

    ai_worker.start();

    // ── 영상 파이프라인 실행 ─────────────────────────────────────
    StatsReporter stats(clients, detections, stream_server);
    VideoPipeline pipeline(stream_server, detections, ai_worker, stats,
                           snapshots, snapshots_fall);
    // [낙상 선택 노출] 전원 블러본 위에 낙상자 얼굴만 되살린 선택본 생성.
    // 관제 Qt 송출·보호자 텔레그램 사진에 쓰이고, Gemini로 가는 버퍼 A는 안 건드린다.
    pipeline.setFallVariant([&](int ch, const cv::Mat& full_blur,
                                const cv::Mat& clean,
                                const std::vector<Detection>& dets,
                                cv::Mat& out) -> bool {
        if (!privacy_masker.hasFall(ch)) return false;
        out = full_blur.clone();
        privacy_masker.restoreFallen(ch, out, clean, dets);
        return true;
    });

    // [블러처리] 송출 전 동적 프라이버시 마스킹 단계
    pipeline.addStage([&](int ch, cv::Mat& img,
                          const std::vector<Detection>& dets) {
        privacy_masker.process(ch, img, dets);
    });

    // 채널별 큐 등록 → run()이 채널마다 전용 처리 스레드를 띄운다.
    for (auto& entry : queues) {
        pipeline.addChannel(entry.first, *entry.second);
    }

    pipeline.run(g_stop);  // 모든 채널 스레드 기동 후 stop까지 블로킹(전체 join)

    // ── 종료 ─────────────────────────────────────────────────────
    std::printf("종료 중...\n");
    telegram.stopPolling();  // [케어봇] 폴링 스레드 join (curl_global_cleanup 전에)
    ai_worker.stop();     // AI 스레드 join
    caregiver.flush();    // 열린 케어 세션 마감 → DB 기록
    blackbox.flushAll();  // 저장 중이던 클립 마무리 (유실 방지)
    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    blackbox.stopHttp();
    curl_global_cleanup();
    return 0;
}