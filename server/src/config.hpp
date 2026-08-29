#pragma once

#include <map>
#include <string>
#include <vector>

#include "protocol/video_stream.h"

// config/cameras.conf 파싱 결과 (공용 — 바뀔 일 거의 없음)
struct CameraConfig {
    int channel;
    std::string url;
};

struct ServerConfig {
    std::vector<CameraConfig> cameras;
    int stream_port = DBJ_VS_PORT_DEFAULT;

    // [TLS] Qt로 나가는 영상 스트림(stream_port) 암호화용 인증서/키.
    // 둘 다 비어있으면 평문으로 동작(기존 동작 유지, 개발 환경 등에서 인증서
    // 없이도 그대로 뜨게 하기 위함). 값이 있는데 로드에 실패하면 서버는 시작하지
    // 않는다 — "TLS를 켰다고 설정했는데 조용히 평문으로 내려가는" 상황을 막기 위해.
    std::string stream_cert_path;
    std::string stream_key_path;
    std::string telegram_bot_token;  // 보호자 알림용 텔레그램 봇 토큰 (데모: 미설정 시 알림 비활성)
    std::string telegram_chat_id;    // 채널별 telegram_chat_id_N이 없을 때 쓰는 기본 수신자

    // ── [수신자 역할 분리] 보호자 / 요양사 ──────────────────────────────
    // 한 방(채널)에 여러 입소자가 누우면 수신자도 갈린다. 그래서 보호자는 두 층으로
    // 잡는다 — "이 방을 볼 권한"(채널)과 "이 사람 사고를 받을 대상"(입소자).
    //
    //   telegram_chat_id_<채널>        : 그 방을 조회할 수 있는 보호자들(콤마로 여럿)
    //   telegram_guardian_chat_id_<입소자id> : 그 입소자 사고를 받을 보호자 1명
    //
    // 낙상처럼 사람이 특정된 사건은 입소자 매핑을 먼저 쓰고, 신원 미상이면 그 방
    // 보호자 전원으로 폴백한다. 방에 2명이 있는데 입소자 매핑이 없으면 "누구 일인지
    // 모른 채 둘 다에게" 가는 게 "엉뚱한 한 명에게만" 가는 것보다 낫기 때문.
    std::map<int, std::vector<std::string>> telegram_chat_ids;      // 채널 → 보호자들
    std::map<int, std::string> telegram_guardian_chat_ids;          // 입소자id → 보호자

    // [알림 정책] 보호자에게 사건 알림(낙상·이탈·생체이상)을 push 할지.
    // 0으로 두면 보호자는 봇으로 조회만 하고 알림은 받지 않는다 — 요양사가 조치
    // 주체이고, 보호자는 원격에서 할 수 있는 게 없어 불안만 커진다는 판단.
    // 요양사 알림은 이 값과 무관하게 항상 나간다.
    bool telegram_guardian_alerts = true;  // 기존 동작 유지가 기본

    // [요양사] 요양사는 담당 방을 지정하지 않고 전 채널을 본다 — 그래서 채널·입소자
    // 접미사가 없고, 여러 명이면 콤마로 나열한다(telegram_staff_chat_id=111,222).
    // ★ 보호자와 갈리는 지점: 무음(🔕)이 적용되지 않아 알림이 항상 나가고, 버튼도
    //   방을 골라 누르는 형태다(TelegramModule::Role 참고).
    std::vector<std::string> telegram_staff_chat_ids;

    // [케어 봇] 실시간 상황 질의응답용 VLM(Gemini) 설정. 미설정 시 봇은 질문에
    // 답하지 못하고 폴백 메시지를 보낸다(서버는 정상 동작).
    std::string gemini_api_key;   // Google AI Studio 발급 키
    // 모델 id는 시기별로 바뀌므로 AI Studio에서 현재 값 확인 권장. 미지정 시 기본값.
    // ★ 예전 기본값이던 gemini-flash-latest(별칭)를 버린 이유 — 2026-08 실측:
    //   그 별칭으로 :generateContent 를 부르면 응답이 1바이트도 오지 않고 그대로
    //   멈춘다(60초 예산을 다 씀). 같은 키·같은 호스트로 모델 목록 GET 은 0.25초에
    //   24KB 를 정상 수신하므로 회선·키 문제가 아니다. 별칭은 "은퇴에 강하다"는
    //   이점이 있지만, 가리키는 대상이 바뀌며 조용히 멈추면 알아챌 방법이 없다 —
    //   404 로 빨리 죽는 고정 id 가 낫다(gemini-2.5-flash 는 실제로 404 를 주며
    //   3.6-flash 를 쓰라고 안내한다).
    std::string gemini_model = "gemini-3.6-flash";

    // [케어 봇] 📞 연락처 버튼이 회신할 연락처. 미설정 시 "미등록"으로 표시.
    std::string care_contact_caregiver;  // 담당 요양사
    std::string care_contact_manager;    // 시설 관리자

    // [DB] MariaDB 호스트. 2-Pi 분할 시 DB를 호스팅하지 않는 Pi는 cameras.conf에
    // db_host=<DB Pi IP> 로 지정한다. 기본은 자기 자신(단일 Pi 또는 DB 호스팅 Pi).
    std::string db_host = "127.0.0.1";

    // [영상검색] 케어봇 검색 결과에 넣을 클립 링크용, 이 Pi 자신의 접속 주소.
    // 서버는 자기 외부/LAN IP를 스스로 모른다(db_host와 같은 이유). 미설정 시
    // 링크 대신 "관제 화면에서 확인" 안내 문구로 대체 — 기능 자체는 죽지 않는다.
    std::string public_host;

    // [NVR] 연속 녹화 저장 경로(USB 마운트 지점 등). 빈 값이면 NVR 비활성 —
    // 서버 나머지 기능(라이브 스트리밍·블랙박스 등)은 그대로 동작한다.
    std::string nvr_storage_path;
    int nvr_retention_hours = 12;   // 이보다 오래된 세그먼트는 자동 삭제
    int nvr_segment_minutes = 10;   // 세그먼트 파일 하나의 길이
    int nvr_http_port = 5502;       // Qt가 NVR 세그먼트 목록/재생을 받아가는 포트

    // ── [데모] MoveNet 관절 오버레이 ─────────────────────────────
    // 1이면 Qt로 나가는 영상에 MoveNet 관절(점·뼈대)과 판정 상태를 그려 보낸다.
    // 시연에서 "AI가 실제로 자세를 보고 있다"를 눈으로 보여주는 용도라 기본은 꺼짐
    // (송출본에만 그린다 — Gemini·텔레그램으로 가는 스냅샷엔 안 들어간다).
    // ★ 켤 때는 아래 두 주기도 같이 줄여야 점이 사람을 따라간다. 운영 기본값은
    //   CPU를 아끼려고 객체당 2초에 한 번만 추론하므로, 점이 2초간 얼어붙는다.
    bool pose_overlay = false;
    // 객체 1명당 자세 추론 주기(초). 0 이하(미설정)면 기본값 2.0 유지.
    // 시연 권장값 0.2~0.3 — 사람이 1~2명일 때만 감당된다(Thunder 추론 ~100ms).
    double pose_interval_sec = 0;
    // 채널당 AI 워커 처리 주기 하한(초). 0 이하(미설정)면 기본값 0.25 유지.
    // pose_interval_sec를 아무리 줄여도 이 값이 상한이라 같이 줄여야 한다.
    double ai_job_interval_sec = 0;
};

// 형식: "채널번호=RTSP URL" 또는 "stream_port=포트", '#' 주석
ServerConfig loadServerConfig(const std::string& path);
