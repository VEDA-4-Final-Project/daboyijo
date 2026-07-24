#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

// 앞뒤 공백·탭·CR(윈도우 줄바꿈) 제거 — URL에 섞이면 RTSP 요청이 깨진다(505 등)
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(ws) - begin + 1);
}

}  // namespace

ServerConfig loadServerConfig(const std::string& path) {
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
        if (key == "telegram_bot_token") {
            config.telegram_bot_token = value;
            continue;
        }
        if (key == "telegram_chat_id") {
            config.telegram_chat_id = value;
            continue;
        }
        if (key == "gemini_api_key") {
            config.gemini_api_key = value;
            continue;
        }
        if (key == "gemini_model") {
            if (!value.empty()) config.gemini_model = value;
            continue;
        }
        static const std::string kChatIdPrefix = "telegram_chat_id_";
        if (key.size() > kChatIdPrefix.size() &&
            key.compare(0, kChatIdPrefix.size(), kChatIdPrefix) == 0) {
            std::string ch_str = key.substr(kChatIdPrefix.size());
            if (!ch_str.empty() &&
                std::all_of(ch_str.begin(), ch_str.end(),
                            [](unsigned char c) { return std::isdigit(c); })) {
                config.telegram_chat_ids[std::stoi(ch_str)] = value;
            }
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
