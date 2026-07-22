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
};

// 형식: "채널번호=RTSP URL" 또는 "stream_port=포트", '#' 주석
ServerConfig loadServerConfig(const std::string& path);
