#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// 앞뒤 공백·탭·CR(윈도우 줄바꿈) 제거 — URL에 섞이면 RTSP 요청이 깨진다(505 등)
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(ws) - begin + 1);
}

// "111,222" → {"111","222"}. 빈 항목은 버린다(끝 콤마·중복 콤마 허용).
// 수신자 키는 사람이 손으로 적는 값이라, 콤마 뒤 공백이 섞여도 그냥 먹도록 trim한다.
std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= value.size()) {
        const size_t comma = value.find(',', pos);
        const std::string one =
            trim(comma == std::string::npos ? value.substr(pos)
                                            : value.substr(pos, comma - pos));
        if (!one.empty()) out.push_back(one);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
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
        if (key == "stream_cert_path") {
            config.stream_cert_path = value;
            continue;
        }
        if (key == "stream_key_path") {
            config.stream_key_path = value;
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
        if (key == "telegram_guardian_alerts") {
            // "0"/"false"/"no"만 끔으로 본다 — 오타로 알림이 조용히 죽는 것보다
            // 오타로 알림이 살아 있는 쪽이 안전하다.
            config.telegram_guardian_alerts =
                !(value == "0" || value == "false" || value == "no");
            continue;
        }
        if (key == "telegram_staff_chat_id") {
            // 요양사는 전 채널 담당이라 접미사가 없다. 여러 명이면 콤마로 나열.
            config.telegram_staff_chat_ids = splitCsv(value);
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
        if (key == "db_host") {
            if (!value.empty()) config.db_host = value;
            continue;
        }
        if (key == "public_host") {
            config.public_host = value;
            continue;
        }
        if (key == "care_contact_caregiver") {
            config.care_contact_caregiver = value;
            continue;
        }
        if (key == "care_contact_manager") {
            config.care_contact_manager = value;
            continue;
        }
        if (key == "nvr_storage_path") {
            config.nvr_storage_path = value;
            continue;
        }
        if (key == "nvr_retention_hours") {
            if (!value.empty()) config.nvr_retention_hours = std::stoi(value);
            continue;
        }
        if (key == "nvr_segment_minutes") {
            if (!value.empty()) config.nvr_segment_minutes = std::stoi(value);
            continue;
        }
        if (key == "nvr_http_port") {
            if (!value.empty()) config.nvr_http_port = std::stoi(value);
            continue;
        }
        if (key == "pose_overlay") {
            // 시연용 스위치 — 명시적으로 켤 때만 켠다(오타면 꺼진 채로).
            config.pose_overlay = (value == "1" || value == "true" || value == "yes");
            continue;
        }
        if (key == "pose_interval_sec") {
            if (!value.empty()) config.pose_interval_sec = std::stod(value);
            continue;
        }
        if (key == "ai_job_interval_sec") {
            if (!value.empty()) config.ai_job_interval_sec = std::stod(value);
            continue;
        }
        // 입소자별 보호자: telegram_guardian_chat_id_<입소자id>=<chat_id>
        // ★ 아래 telegram_chat_id_ 검사보다 먼저 와야 한다 — 접두사가 겹치지는
        //   않지만, 키를 읽는 순서가 곧 우선순위라는 걸 눈에 보이게 두려는 것.
        static const std::string kGuardianPrefix = "telegram_guardian_chat_id_";
        if (key.size() > kGuardianPrefix.size() &&
            key.compare(0, kGuardianPrefix.size(), kGuardianPrefix) == 0) {
            const std::string rid_str = key.substr(kGuardianPrefix.size());
            if (!rid_str.empty() &&
                std::all_of(rid_str.begin(), rid_str.end(),
                            [](unsigned char c) { return std::isdigit(c); })) {
                config.telegram_guardian_chat_ids[std::stoi(rid_str)] = value;
            }
            continue;
        }
        // 채널별 보호자: telegram_chat_id_<채널>=<chat_id>[,<chat_id>...]
        // 한 방에 여러 입소자가 있으면 보호자도 여럿이라 콤마로 나열할 수 있다.
        // 기존 단일값 설정은 그대로 원소 1개짜리로 읽히므로 호환이 깨지지 않는다.
        static const std::string kChatIdPrefix = "telegram_chat_id_";
        if (key.size() > kChatIdPrefix.size() &&
            key.compare(0, kChatIdPrefix.size(), kChatIdPrefix) == 0) {
            std::string ch_str = key.substr(kChatIdPrefix.size());
            if (!ch_str.empty() &&
                std::all_of(ch_str.begin(), ch_str.end(),
                            [](unsigned char c) { return std::isdigit(c); })) {
                config.telegram_chat_ids[std::stoi(ch_str)] = splitCsv(value);
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
