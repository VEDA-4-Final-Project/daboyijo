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

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
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
#include "sharpen_enhancer.hpp"
#include "protocol/video_stream.h"
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
    if (config.cameras.empty()) return 1;

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
    DetectionStore detections;  // 감지 이력 저장 + 프레임-좌표 시간 매칭
    SnapshotBuffer snapshots;   // 버퍼 A: 전원 블러본 (Gemini/평상시 사진용)
    SnapshotBuffer snapshots_fall;  // 버퍼 B: 낙상 선택본 (낙상자만 노출, 보호자 사진용)
    AiWorker ai_worker;         // 무거운 AI 연산 전담 — 채널별 워커 스레드
    Database db;

    // ── 기능 모듈 생성 ───────────────────────────────────────────
    FallModule fall;                // [낙상감지]
    BedEgressModule bed_egress;     // [침상탈출]
    PrivacyMasker privacy_masker;   // [블러처리]
    // [선명도 보정] 사람 영역만 샤프닝. amount=강도, sigma=윤곽 반경(작을수록 쨍함).
    // 눈에 잘 띄도록 강하게: 세부 윤곽을 또렷하게 세운다. 과하면 노이즈·헤일로가
    // 보일 수 있으니 화면 보며 조절할 것 (은은하게: (0.5, 3.0)).
    // [테스트 2026-07-28] 부하 완화 실험으로 샤픈 stage 비활성 → 선언도 잠시 주석
    //   (미사용 경고 방지). 되살리려면 이 줄과 아래 addStage 블록을 함께 주석 해제.
    // SharpenEnhancer sharpen_enhancer(1.2, 1.0);
    CaregiverModule caregiver(db);  // [요양사감지]
    BlackboxModule blackbox;        // [블랙박스]
    TelegramModule telegram;        // [보호자 알림 + 케어봇]
    telegram.configure(config.telegram_bot_token, config.telegram_chat_id, config.telegram_chat_ids);

    // [케어봇] 실시간 상황 질의응답: 보호자 질문 → 스냅샷 + VLM → 답변
    GeminiClient vlm(config.gemini_api_key, config.gemini_model);
    CareQaModule care_qa(snapshots, snapshots_fall, vlm, telegram, [&](int ch) {
        privacy_masker.clearFall(ch);  // 봇 "/확인" → 낙상 블러 원상복구
        std::printf("ch%d 낙상 경보 확인(텔레그램).\n", ch + 1);
    });
    telegram.setCommandHandler([&](int ch, const std::string& chat_id,
                                   const std::string& text) {
        care_qa.handleMessage(ch, chat_id, text);
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

    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        // 이 채널 전용 큐 생성 (용량 8 — 버스티 디코딩 출력을 흡수해 드랍 방지.
        // 소비자가 빠르면 큐는 거의 비어 있어 지연은 낮게 유지된다).
        auto& cam_queue =
            *(queues[cam.channel] = std::make_unique<FrameQueue>(8));
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
        client->start();
        clients.push_back(std::move(client));
    }
    std::printf("%zu개 채널 수신 시작 (Ctrl+C로 종료)\n", clients.size());

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
    // [선명도 보정] 사람(Human) 영역만 샤프닝.
    // ★ 반드시 블러 stage보다 "앞"에 둔다: 몸통을 먼저 선명하게 만든 뒤
    //   그 위에 얼굴 블러가 덮여야 프라이버시가 깨지지 않는다.
    // [테스트 2026-07-28] 부하 완화 실험 — 샤픈은 미용 기능이라 CPU 급할 때 후보.
    //   큰 사람 박스 GaussianBlur가 비쌈. 되살리려면 아래 블록 주석 해제 + 위 선언부.
    // pipeline.addStage([&](int ch, cv::Mat& img,
    //                       const std::vector<Detection>& dets) {
    //     sharpen_enhancer.process(ch, img, dets);
    // });

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