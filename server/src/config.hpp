#pragma once

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
};

// 형식: "채널번호=RTSP URL" 또는 "stream_port=포트", '#' 주석
ServerConfig loadServerConfig(const std::string& path);
