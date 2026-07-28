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
    std::string telegram_bot_token;  // 보호자 알림용 텔레그램 봇 토큰 (데모: 미설정 시 알림 비활성)
    std::string telegram_chat_id;    // 채널별 telegram_chat_id_N이 없을 때 쓰는 기본 수신자
    std::map<int, std::string> telegram_chat_ids;  // 채널별 보호자 chat_id (telegram_chat_id_N)

    // [케어 봇] 실시간 상황 질의응답용 VLM(Gemini) 설정. 미설정 시 봇은 질문에
    // 답하지 못하고 폴백 메시지를 보낸다(서버는 정상 동작).
    std::string gemini_api_key;   // Google AI Studio 발급 키
    // 모델 id는 시기별로 바뀌므로 AI Studio에서 현재 값 확인 권장. 미지정 시 기본값.
    // gemini-flash-latest = 항상 현재 안정 Flash를 가리키는 별칭(모델 은퇴에 강함).
    std::string gemini_model = "gemini-flash-latest";

    // [DB] MariaDB 호스트. 2-Pi 분할 시 DB를 호스팅하지 않는 Pi는 cameras.conf에
    // db_host=<DB Pi IP> 로 지정한다. 기본은 자기 자신(단일 Pi 또는 DB 호스팅 Pi).
    std::string db_host = "127.0.0.1";
};

// 형식: "채널번호=RTSP URL" 또는 "stream_port=포트", '#' 주석
ServerConfig loadServerConfig(const std::string& path);
