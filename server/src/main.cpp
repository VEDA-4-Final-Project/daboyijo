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

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

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
#include "rtsp_av_client.hpp"
#include "stats_reporter.hpp"
#include "stream_server.hpp"
#include "video_pipeline.hpp"
#include "bed_egress_module.hpp"

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

    // ── 공용 인프라 ──────────────────────────────────────────────
    StreamServer stream_server(config.stream_port);
    FrameQueue queue(16);
    DetectionStore detections;  // 감지 이력 저장 + 프레임-좌표 시간 매칭
    AiWorker ai_worker;         // 무거운 AI 연산 전담 — 채널별 워커 스레드
    Database db;

    // ── 기능 모듈 생성 ───────────────────────────────────────────
    FallModule fall;                // [낙상감지]
    BedEgressModule bed_egress;     // [침상탈출]
    PrivacyMasker privacy_masker;   // [블러처리]
    CaregiverModule caregiver(db);  // [요양사감지]
    BlackboxModule blackbox;        // [블랙박스]

    // ── 모듈 간 배선 ─────────────────────────────────────────────
    // Qt가 그린 침대 ROI → 낙상 및 침상 탈출 판정기
    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        fall.updateBedRoi(up.channel, up.clear, up.points);
        bed_egress.updateBedRoi(up.channel, up.clear, up.points);
    });
    // Qt의 낙상 확인 신호 → 블러 원상복구
    stream_server.setConfirmCallback([&](int ch) {
        privacy_masker.clearFall(ch);
        std::printf("ch%d 낙상 경보 확인.\n", ch);
    });
    // 낙상 확정 → 블러 즉시 해제 + 블랙박스 클립 저장 + Qt 경보
    fall.setFallCallback([&](int ch, const Detection& at) {
        std::fprintf(stderr, "🚨 [ch%d] 낙상 의심! (자세 판정) cx=%.2f cy=%.2f\n",
                     ch, at.cx, at.cy);
        privacy_masker.reportFall(ch);
        int64_t evt_ms = blackbox.trigger(ch);
        stream_server.broadcastEvent(ch, DBJ_EVT_FALL, at.cx, at.cy, evt_ms);
    });
    // 침상 탈출 -> 블랙박스 클립 저장 + Qt 경보
    bed_egress.setAlarmCallback([&](int ch, int obj_id) {
        std::fprintf(stderr, "⚠️ [ch%d] 환자 침상 탈출 감지! (obj: %d)\n", ch, obj_id);
        int64_t evt_ms = blackbox.trigger(ch);
        stream_server.broadcastEvent(ch, DBJ_EVT_EGRESS, 0.0f, 0.0f, evt_ms); 
    });
    // AI 워커에 분석 프로세서 등록 (실행 순서 = 등록 순서)
    ai_worker.addProcessor([&](const AiJob& job) { caregiver.processFrame(job); });
    ai_worker.addProcessor([&](const AiJob& job) { fall.processFrame(job); });

    // ── 서버 기동 ────────────────────────────────────────────────
    if (!stream_server.start()) return 1;
    blackbox.startHttp();
    db.connect("127.0.0.1", "daboijo", "1234", "daboijo");

    std::vector<std::unique_ptr<RtspAvClient>> clients;
    for (const auto& cam : config.cameras) {
        auto client = std::make_unique<RtspAvClient>(cam.channel, cam.url, queue);
        client->setDetectionCallback([&](int ch, std::vector<Detection> dets) {
            fall.onMetadata(ch, dets);             // 낙상: ROI 게이팅 + bbox 캐시
            bed_egress.processDetections(ch, dets);// 침상
            detections.push(ch, std::move(dets));  // 공용: 시간 매칭용 이력 저장
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
    VideoPipeline pipeline(queue, stream_server, detections, ai_worker, stats);
    // [블러처리] 송출 전 동적 프라이버시 마스킹 단계
    pipeline.addStage([&](int ch, cv::Mat& img,
                          const std::vector<Detection>& dets) {
        privacy_masker.process(ch, img, dets);
    });

    pipeline.run(g_stop);

    // ── 종료 ─────────────────────────────────────────────────────
    std::printf("종료 중...\n");
    ai_worker.stop();     // AI 스레드 join
    caregiver.flush();    // 열린 케어 세션 마감 → DB 기록
    blackbox.flushAll();  // 저장 중이던 클립 마무리 (유실 방지)
    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    blackbox.stopHttp();
    return 0;
}
