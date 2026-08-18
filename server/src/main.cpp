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
#include "bed_zones.hpp"
#include "blackbox_module.hpp"
#include "nvr_module.hpp"
#include "caregiver_module.hpp"
#include "config.hpp"
#include "database.hpp"
#include "detection_store.hpp"
#include "fall_module.hpp"
#include "identity_tracker.hpp"
#include "frame_queue.hpp"
#include "privacy_masker.hpp"
#include "protocol/video_stream.h"
#include "onvif_imaging.hpp"
#include "sunapi_focus.hpp"
#include "rtsp_av_client.hpp"
#include "stats_reporter.hpp"
#include "stream_server.hpp"
#include "video_pipeline.hpp"
#include "bed_egress_module.hpp"
#include "telegram_module.hpp"
#include "snapshot_buffer.hpp"
#include "gemini_client.hpp"
#include "video_search_module.hpp"
#include "care_qa.hpp"
#include "MqttMasterManager.hpp"
#include "activity_module.hpp"

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
    // stream_cert_path/stream_key_path가 비어 있으면 평문(기존 동작) — 개발 환경
    // 등 인증서를 안 놓은 곳에서도 그대로 뜬다. 값이 있는데 로드 실패하면 start()가
    // false를 반환해 아래에서 바로 종료한다.
    StreamServer stream_server(config.stream_port, config.stream_cert_path,
                                config.stream_key_path);
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
    // 침대 ROI는 낙상·침상이탈·객체 귀속이 같은 값을 봐야 해서 여기 한 곳에만 둔다.
    // 한 채널(=한 병실 시야)에 침대가 여럿이고, 침대마다 입소자가 다르다.
    BedZoneStore bed_zones;
    // 추적 객체(object_id) ↔ 침대 ↔ 입소자 귀속 — 알림에 "누가"를 붙이는 근거.
    // 원리와 한계는 core/identity_tracker.hpp 머리 주석 참조.
    IdentityTracker identity;

    FallModule fall;                // [낙상감지]
    BedEgressModule bed_egress;     // [침상탈출]
    fall.setBedZones(&bed_zones);
    bed_egress.setBedZones(&bed_zones);
    // [일일 리포트] 침대 재실 시간(bed_sessions) 기록. identity 는 요양보호사를
    // 재실에서 빼기 위한 것 — 보호사가 침대에 걸터앉으면 발끝이 ROI 안으로 들어온다.
    bed_egress.setDatabase(&db);
    bed_egress.setIdentity(&identity);
    PrivacyMasker privacy_masker;   // [블러처리]
    CaregiverModule caregiver(db);  // [요양사감지]
    BlackboxModule blackbox;        // [블랙박스]
    NvrModule nvr(config.nvr_storage_path, config.nvr_retention_hours,
                  config.nvr_segment_minutes, config.nvr_http_port);  // [NVR 연속녹화]
    TelegramModule telegram;        // [보호자 알림 + 케어봇]
    telegram.configure(config.telegram_bot_token, config.telegram_chat_id, config.telegram_chat_ids);

    ActivityModule activity(db);    // [일일 리포트] 만보기 → activity_minute
    MqttMasterManager mqtt;         // [mqtt]
    // 실패해도 서버는 계속 뜬다(영상·낙상 판정은 MQTT와 무관) — 다만 그 사이
    // 알람은 알림 노드에 닿지 않으므로, 조용히 넘기지 말고 크게 남긴다.
    // 연결은 백그라운드에서 계속 재시도된다(MqttMasterManager::init 참고).
    if (!mqtt.init("dabo.local", 8883)) {
        std::fprintf(stderr,
                     "[main] MQTT 브로커 미연결 상태로 시작합니다 — 웨어러블 수신과 "
                     "알림 노드 알람은 재접속될 때까지 동작하지 않습니다.\n");
    }

    // [케어봇] 버튼 메뉴 기반 상호작용: 보호자가 아무 메시지나 보내면 버튼 메뉴를
    // 띄우고(handleMessage), 버튼 클릭(handleCallback)으로 상황 조회·연락처·알림 토글.
    // ※ 낙상 확인(블러 원복)은 텔레그램에서 빼고 Qt 관제 화면(setConfirmCallback)만 담당.

    GeminiClient vlm(config.gemini_api_key, config.gemini_model);
    // [영상검색] 🔍 버튼이 위임하는 자연어 질의 처리기. vlm(GeminiClient)을 그대로
    // 재사용 — 질의 파싱은 이미지가 필요 없는 텍스트 전용 VlmClient::ask() 사용.
    VideoSearchModule video_search(vlm, db, config.public_host);
    CareQaModule care_qa(snapshots, snapshots_fall, vlm, telegram, video_search);
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
    // Qt가 그린 침대 ROI → 공용 보관소(낙상·침상탈출·귀속이 같이 본다) + DB 영속화.
    // ★ DB에도 남기는 이유: 침대에 입소자까지 매핑하는 지금은 서버를 재시작할 때마다
    //   관리자가 4채널 침대를 다시 그리고 사람을 다시 지정할 수 없다.
    stream_server.setRoiCallback([&](const StreamServer::RoiUpdate& up) {
        const bool all = (up.roi_id == BedZoneStore::kRoiIdAll);
        bed_zones.update(up.channel, up.roi_id, up.clear, up.points);
        if (up.clear) {
            bed_egress.resetZoneState(up.channel, up.roi_id);
            db.deleteRoiZone(up.channel, all ? -1 : up.roi_id);
            std::printf("ch%d 침대 %s 삭제\n", up.channel + 1,
                        all ? "전체" : ("#" + std::to_string(up.roi_id + 1)).c_str());
        } else {
            db.saveRoiZone(up.channel, up.roi_id, up.points);
            std::printf("ch%d 침대%d ROI 갱신 (%zu점)\n", up.channel + 1,
                        up.roi_id + 1, up.points.size());
        }
    });
    // Qt의 "이 침대는 이 사람 자리" 지정 → 귀속 앵커 갱신 + DB 영속화.
    stream_server.setRoiBindCallback([&](int ch, int roi_id, int resident_id) {
        bed_zones.bind(ch, roi_id, resident_id);
        db.bindRoiZoneResident(ch, roi_id, resident_id);
        // 사람이 바뀌면 위험도도 그 사람 것으로 따라가야 한다.
        int level = db.getRiskLevelByResident(resident_id);
        if (level >= 1 && level <= 3) bed_egress.updatePatientStatus(ch, roi_id, level);
        std::printf("ch%d 침대%d ← 입소자 %d 매핑\n", ch + 1, roi_id + 1, resident_id);
    });
    // Qt의 낙상 확인 신호 → 블러 원상복구
    stream_server.setConfirmCallback([&](int ch) {
        privacy_masker.clearFall(ch);
        std::printf("ch%d 낙상 경보 확인.\n", ch + 1);
    });
    //  Qt의 환자 정보 변경 신호 → 침상 탈출 모듈의 환자 관리 상태 갱신(인메모리).
    //  DB 영속화는 Qt가 residents.risk_level에 직접 기록하므로 서버는 하지 않는다
    //  (부팅 시 bed_egress.initializeFromDb → getRiskLevelByCamera로 복원).
    stream_server.setRiskLevelCallback([&](int ch, int roi_id, int patient_status) {
        bed_egress.updatePatientStatus(ch, roi_id, patient_status);
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
                if (applyOnvifImaging(url, ch, p, &err))
                    std::fprintf(stderr, "[image] ch%d 이미지 적용 성공\n", ch + 1);
                else
                    std::fprintf(stderr, "[image] ch%d 이미지 적용 실패: %s\n",
                                 ch + 1, err.c_str());
            }).detach();
        });
    // Qt의 "포커스" 신호 → 해당 채널 카메라에 SUNAPI SimpleFocus 적용.
    // area=false 전체 자동초점, area=true 클릭 지점(정규화 좌표) 영역 초점.
    stream_server.setFocusCallback(
        [&](int ch, bool area, float nx, float ny) {
            if (ch < 0 || ch >= 4) return;
            std::string url;
            {
                std::lock_guard<std::mutex> lk(cam_url_mutex);
                url = cam_url_by_channel[ch];
            }
            if (url.empty()) {
                std::fprintf(stderr, "[focus] ch%d 카메라 미연결 — 초점 무시\n", ch + 1);
                return;
            }
            std::thread([ch, url, area, nx, ny]() {
                std::string err;
                if (sunapiFocus(url, ch, area, nx, ny, &err))
                    std::fprintf(stderr, "[focus] ch%d 초점 적용 성공%s\n", ch + 1,
                                 area ? " (영역)" : " (전체)");
                else
                    std::fprintf(stderr, "[focus] ch%d 초점 적용 실패: %s\n",
                                 ch + 1, err.c_str());
            }).detach();
        });
    // [영상검색] Qt 관제 화면 "🔍 영상 검색"에서 온 자연어 질의 → video_search로
    // 처리(Gemini+DB 왕복, 수 초) → 그 클라이언트에게만 회신. 케어봇(텔레그램)과
    // 같은 video_search 인스턴스를 재사용 — 로직은 한 곳(video_search_module)뿐.
    stream_server.setSearchQueryCallback(
        [&](int ch, uint64_t clientId, const std::string& query) {
            std::thread([&stream_server, &video_search, ch, clientId, query]() {
                const std::string answer = video_search.search(ch, query);
                stream_server.sendSearchResult(clientId, ch, answer);
            }).detach();
        });
    // CCTV 기반 낙상 판정  → 블러 부분 해제 + 블랙박스 클립 저장 + Qt 경보
    fall.setFallCallback([&](int ch, const Detection& at) {
        // 낙상한 사람은 정의상 침대 밖이라 "지금 어느 침대 안인가"로는 누구인지
        // 알 수 없다. 그래서 이 트랙이 원래 어느 침대 사람이었는지를 물어본다.
        const auto who = identity.identify(ch, at.object_id);
        const int roi_id = who.named() ? who.roi_id : -1;
        const std::string name =
            who.named() ? db.getResidentName(who.resident_id) : std::string();

        std::fprintf(stderr,
                     "🚨 [ch%d] 낙상 의심! (CCTV 판정) obj=%d cx=%.2f cy=%.2f — %s\n",
                     ch + 1, at.object_id, at.cx, at.cy,
                     name.empty()
                         ? "신원 미상"
                         : (name + " (침대" + std::to_string(roi_id + 1) + ", 신뢰도 " +
                            std::to_string(int(who.confidence * 100)) + "%)").c_str());

        privacy_masker.reportFall(ch, at.object_id, at.cx, at.cy);
        int64_t evt_ms = blackbox.trigger(ch, "FALL");
        stream_server.broadcastEvent(ch, DBJ_EVT_FALL, at.cx, at.cy, roi_id, evt_ms);
        telegram.notifyFall(ch);      // 즉시 기본 알림
        care_qa.reportFall(ch);       // [케어봇] 몇 초 뒤 VLM 상황 설명+스냅샷 자동 전송
        // 호실은 사람이 특정되면 그 사람 것, 아니면 예전처럼 채널 대표값.
        int room = who.named() ? db.getRoomByResident(who.resident_id) : -1;
        if (room < 0) room = db.getRoomByCh(ch);
        mqtt.sendAlarmCommand(AlarmEventType::FALL,room); // mqtt 알림전송
        // [일일 리포트] 이벤트 원장에 기록. clip 은 파일명만 남긴다 —
        // 서버는 자기 외부 IP 를 모르고, Qt 는 채널별 호스트를 이미 안다.
        //
        // ★ 사람은 IdentityTracker 가 정한 값을 그대로 넘긴다. 넘기지 않으면
        //   insertEvent 가 "이 채널의 재원자" 로 되짚는데, 한 방에 여러 명이면
        //   그중 첫 사람으로 붙어 절반이 조용히 틀린다.
        //   신뢰도가 낮으면(named()==false) 0 을 넘겨 resident_id 를 NULL(미상)로
        //   남긴다 — 잘못된 이름은 이름 없는 것보다 나쁘다. (-1 은 "카메라로 찾아라"
        //   라서 여기서 쓰면 방금 미상으로 판정한 걸 도로 추측하게 된다)
        char clip[64];
        std::snprintf(clip, sizeof(clip), "ch%d_%lld_FALL.mp4", ch, (long long)evt_ms);
        db.insertEvent(EventType::Fall, EventSource::Camera, ch, evt_ms, clip,
                       who.named() ? who.resident_id : 0);
    });
    // CCTV 기반 침상 탈출 -> 블랙박스 클립 저장 + Qt 경보
    bed_egress.setAlarmCallback([&](const EgressEvent& e) {
        // 이탈은 "어느 침대에서 나갔는지"가 판정 자체에 들어 있어서, 그 침대에
        // 매핑된 입소자가 곧 누구인지다 — 낙상과 달리 추정이 필요 없다.
        const std::string name =
            e.resident_id > 0 ? db.getResidentName(e.resident_id) : std::string();
        std::fprintf(stderr, "⚠️ [ch%d] 침대%d %s 침상 탈출 감지! (obj: %d)\n",
                     e.channel + 1, e.roi_id + 1,
                     name.empty() ? "미지정 입소자" : name.c_str(), e.object_id);

        int64_t evt_ms = blackbox.trigger(e.channel, "EGRESS");
        stream_server.broadcastEvent(e.channel, DBJ_EVT_EGRESS, e.x, e.y, e.roi_id,
                                     evt_ms);
        telegram.notifyEgress(e.channel);
        int room = e.resident_id > 0 ? db.getRoomByResident(e.resident_id) : -1;
        if (room < 0) room = db.getRoomByCh(e.channel);
        mqtt.sendAlarmCommand(AlarmEventType::EGRESS,room);
        // [일일 리포트] 이벤트 원장에 기록.
        // ★ 이탈은 어느 침대에서 나갔는지가 판정 자체라 사람이 이미 확정돼 있다
        //   (EgressEvent.resident_id). 침대에 입소자를 아직 매핑하지 않았으면 0 이고,
        //   그때는 NULL(미상)로 남는다 — 채널로 되짚지 않는다.
        char eclip[64];
        std::snprintf(eclip, sizeof(eclip), "ch%d_%lld_EGRESS.mp4",
                      e.channel, (long long)evt_ms);
        db.insertEvent(EventType::BedEgress, EventSource::Camera, e.channel, evt_ms,
                       eclip, e.resident_id);
    });
    // 이 서버가 담당하는 채널인가 — cameras.conf 에 적힌 채널만 처리한다.
    //
    // 예전엔 여기가 ch < 2 하드코딩이라 두 가지가 동시에 깨져 있었다:
    //   · 채널 2·3 입소자의 웨어러블 낙상·생체이상이 통째로 무시됐다(알람도 DB도 없음).
    //   · 미등록 웨어러블은 getCHById 가 -1 을 주는데 -1 < 2 가 참이라 ch=-1 로
    //     처리가 진행됐다(broadcastEvent 에서 채널이 255 로 뭉개짐).
    // conf 기준으로 바꾸면 2-Pi 분할에서도 각 Pi 가 자기 채널만 맡아, 같은 브로커를
    // 둘이 구독해도 이벤트가 DB 에 두 번 들어가지 않는다.
    auto handlesChannel = [&config](int ch) {
        for (const auto& cam : config.cameras)
            if (cam.channel == ch) return true;
        return false;
    };

    // 요양보호사로 판정된 객체 → 귀속기에 표시(침대에 들어가도 환자로 안 침).
    caregiver.setCaregiverIdsCallback([&](int ch, const std::vector<int>& ids) {
        identity.markCaregivers(ch, ids);
    });
    // 웨어러블 기반 이벤트 발생
    mqtt.setWearableCallback([&](AlarmEventType event, std::string device_id){
        int ch = db.getCHById(device_id);
        int room = db.getRoomById(device_id);
        // [일일 리포트] 웨어러블은 사람마다 다르므로 카메라와 달리 입소자를
        // 정확히 특정할 수 있다. 아래 insertEvent 에 이 값을 넘긴다.
        int rid = db.getResidentByWearable(device_id);
        // 남의 Pi 담당 채널이면 조용히 넘기는 게 맞지만, 미등록 웨어러블(ch=-1)은
        // 설정 실수라 알려야 한다 — 조용히 버리면 "왜 알람이 안 오지"로 며칠 샌다.
        if(ch < 0)
            std::fprintf(stderr, "[main] ⚠ 등록되지 않은 웨어러블 '%s' — 이벤트 무시."
                                 " residents.wearable_id 를 확인하세요.\n", device_id.c_str());
        if(handlesChannel(ch)){
            // 웨어러블 기반 낙상 판정 -> 블러 전체 해제 + 블랙박스 클립 저장 + Qt 경보
            if(event == AlarmEventType::FALL){
                std::fprintf(stderr, "🚨 [ch%d] 낙상 의심! (웨어러블 판정)\n", ch + 1);
                // privacy_masker.reportFall(ch, at.object_id, at.cx, at.cy); 블러 해제(원본) 추가 예정
                int64_t evt_ms = blackbox.trigger(ch, "FALL");
                // 웨어러블은 화면 좌표도 침대 번호도 모른다 — 미상으로 내려보낸다
                stream_server.broadcastEvent(ch, DBJ_EVT_FALL, 0.0f, 0.0f,
                                             DBJ_ROI_ID_NONE, evt_ms);
                telegram.notifyFall(ch);
                care_qa.reportFall(ch);
                mqtt.sendAlarmCommand(AlarmEventType::FALL,room);
                // ★ 같은 낙상이 카메라 경로로도 들어오면 두 줄이 남는다. 일부러
                //   지우지 않는다 — source 로 구분되고, 어느 쪽이 먼저 잡았는지가
                //   튜닝에 필요하다. 리포트에서 "낙상 N회" 를 셀 때 짧은 시간창
                //   안의 같은 사람 건은 1회로 합칠 것.
                char clip[64];
                std::snprintf(clip, sizeof(clip), "ch%d_%lld_FALL.mp4", ch, (long long)evt_ms);
                db.insertEvent(EventType::Fall, EventSource::Wearable, ch, evt_ms, clip, rid);
            }
            // 웨어러블 기반 생체데이터 이상 -> 블랙박스 클립 저장 + Qt 경보
            else if(event == AlarmEventType::VITAL_ABNORMAL){
                std::fprintf(stderr, "🚨 [ch%d] 생체데이터 이상!\n", ch + 1);
                int64_t evt_ms = blackbox.trigger(ch, "VITAL_ABNORMAL");
                stream_server.broadcastEvent(ch, DBJ_EVT_VITAL_ABNORMAL, 0.0f, 0.0f,
                                             DBJ_ROI_ID_NONE, evt_ms);
                telegram.notifyVitalAbnormal(ch);
                mqtt.sendAlarmCommand(AlarmEventType::VITAL_ABNORMAL,room);
                char clip[64];
                std::snprintf(clip, sizeof(clip), "ch%d_%lld_VITAL_ABNORMAL.mp4",
                              ch, (long long)evt_ms);
                db.insertEvent(EventType::VitalAbnormal, EventSource::Wearable,
                               ch, evt_ms, clip, rid);
            }
        }
    });
    // [일일 리포트] 만보기 활동량 — 알람과 무관하게 모든 패킷을 받아 1분 단위로 집계.
    mqtt.setWearableDataCallback([&](const WearableData& d){ activity.onWearableData(d); });
    // AI 워커에 분석 프로세서 등록 (실행 순서 = 등록 순서)
    ai_worker.addProcessor([&](const AiJob& job) { caregiver.processFrame(job); });
    ai_worker.addProcessor([&](const AiJob& job) { fall.processFrame(job); });

    // ── 서버 기동 ────────────────────────────────────────────────
    if (!stream_server.start()) return 1;
    blackbox.startHttp();
    nvr.startHttp();
    telegram.startPolling();  // [케어봇] getUpdates 롱폴링 스레드 기동
    db.connect(config.db_host, "daboijo", "1234", "daboijo");

    // 침대 ROI·입소자 매핑 복원 — Qt가 다시 접속하지 않아도 판정이 바로 돈다.
    // ★ 위험도 로드보다 먼저다: initializeFromDb가 침대에 매핑된 입소자에게서
    //   위험도를 읽으므로 침대가 먼저 서 있어야 한다.
    for (const auto& z : db.loadRoiZones()) {
        if (z.camera_id < 0 || z.camera_id >= 4) continue;
        if (z.resident_id > 0) bed_zones.bind(z.camera_id, z.roi_id, z.resident_id);
        if (z.points.size() >= 3)
            bed_zones.update(z.camera_id, z.roi_id, false, z.points);
        std::fprintf(stderr, "[main] 침대 복원: ch%d 침대%d (%zu점, 입소자 %d)\n",
                     z.camera_id + 1, z.roi_id + 1, z.points.size(), z.resident_id);
    }
    bed_egress.initializeFromDb(db);
    // [일일 리포트] 지난 실행에서 열린 채 남은 재실 세션 정리. 서버가 죽으면
    // 메모리의 session_id 가 사라져 그 행은 영영 안 닫히고, 리포트는 그걸
    // "지금까지 계속 누워있음"으로 읽어 며칠짜리 재실시간을 만들어낸다.
    db.closeDanglingBedSessions();

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
            // 귀속을 먼저 — 아래 판정들이 낸 이벤트에 "누구"를 붙이려면 이 프레임의
            // 귀속 결과가 이미 반영돼 있어야 한다.
            identity.update(ch, dets, bed_zones.zones(ch));
            fall.onMetadata(ch, dets);             // 낙상: ROI 게이팅 + bbox 캐시
            bed_egress.processDetections(ch, dets);// 침상
            detections.push(ch, std::move(dets), cap);  // 공용: 시간 매칭용 이력 저장
        });
        blackbox.attachChannel(*client);    // 블랙박스: 압축 패킷 버퍼링 배선
        nvr.attachChannel(*client);         // NVR: 연속 녹화 배선
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
    activity.flushAll();  // 아직 안 쓴 마지막 1분치 활동량 저장
    bed_egress.flushBedSessions();  // 열린 재실 세션 마감 → 누워있던 시간 유실 방지
    blackbox.flushAll();  // 저장 중이던 클립 마무리 (유실 방지)
    nvr.flushAll();       // 진행 중이던 NVR 세그먼트 마무리 (유실 방지)
    for (auto& client : clients) {
        client->stop();
    }
    stream_server.stop();
    blackbox.stopHttp();
    nvr.stopHttp();
    curl_global_cleanup();
    return 0;
}
