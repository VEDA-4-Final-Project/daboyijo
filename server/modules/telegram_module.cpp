#include "telegram_module.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

// libcurl이 응답 본문을 받으면 호출 — 알림 전송에는 응답 내용이 필요 없어 버린다.
size_t discardWrite(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

size_t collectWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

TelegramModule::~TelegramModule() { stopPolling(); }

void TelegramModule::configure(std::string bot_token, std::string default_chat_id,
                                std::map<int, std::string> chat_ids_by_channel) {
    bot_token_ = std::move(bot_token);
    default_chat_id_ = std::move(default_chat_id);
    chat_ids_by_channel_ = std::move(chat_ids_by_channel);
}

const std::string& TelegramModule::resolveChatId(int channel) const {
    auto it = chat_ids_by_channel_.find(channel);
    return it != chat_ids_by_channel_.end() ? it->second : default_chat_id_;
}

int TelegramModule::resolveChannel(const std::string& chat_id) const {
    for (const auto& [ch, id] : chat_ids_by_channel_) {
        if (id == chat_id) return ch;
    }
    return -1;  // 기본 chat_id거나 미등록 — 방을 특정할 수 없음
}

bool TelegramModule::isAuthorized(const std::string& chat_id) const {
    if (chat_id.empty()) return false;
    if (!default_chat_id_.empty() && chat_id == default_chat_id_) return true;
    for (const auto& [ch, id] : chat_ids_by_channel_) {
        (void)ch;
        if (id == chat_id) return true;
    }
    return false;
}

void TelegramModule::notifyFall(int channel) {
    sendAsync(channel, "🚨 [ch" + std::to_string(channel + 1) + "] 낙상이 감지되었습니다. 확인 바랍니다.");
}

void TelegramModule::notifyEgress(int channel) {
    sendAsync(channel, "⚠️ [ch" + std::to_string(channel + 1) + "] 침상 이탈이 감지되었습니다.");
}

void TelegramModule::sendAsync(int channel, std::string text) const {
    const std::string& chat_id_ref = resolveChatId(channel);
    if (bot_token_.empty() || chat_id_ref.empty()) return;  // 미설정 시 무시(데모 편의)

    // RTSP/AI 워커 스레드에서 호출되므로, HTTPS 왕복 지연이 실시간 파이프라인을
    // 막지 않도록 별도 스레드에서 전송하고 결과를 기다리지 않는다.
    std::thread([token = bot_token_, chat_id = chat_id_ref, text = std::move(text)]() {
        CURL* curl = curl_easy_init();
        if (!curl) return;

        char* chat_id_esc = curl_easy_escape(curl, chat_id.c_str(), 0);
        char* text_esc = curl_easy_escape(curl, text.c_str(), 0);
        std::string fields = "chat_id=" + std::string(chat_id_esc) + "&text=" + std::string(text_esc);
        curl_free(chat_id_esc);
        curl_free(text_esc);

        std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWrite);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::fprintf(stderr, "[텔레그램] 전송 실패: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }).detach();
}

// ── 봇 응답: 특정 chat_id로 직접 전송 (동기, 호출자 스레드에서 왕복) ──
void TelegramModule::sendMessage(const std::string& chat_id,
                                 const std::string& text) const {
    if (bot_token_.empty() || chat_id.empty()) return;
    CURL* curl = curl_easy_init();
    if (!curl) return;

    char* chat_id_esc = curl_easy_escape(curl, chat_id.c_str(), 0);
    char* text_esc = curl_easy_escape(curl, text.c_str(), 0);
    std::string fields = "chat_id=" + std::string(chat_id_esc) + "&text=" + std::string(text_esc);
    curl_free(chat_id_esc);
    curl_free(text_esc);

    std::string url = "https://api.telegram.org/bot" + bot_token_ + "/sendMessage";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWrite);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
}

void TelegramModule::sendPhoto(const std::string& chat_id,
                               const std::vector<unsigned char>& jpeg,
                               const std::string& caption) const {
    if (bot_token_.empty() || chat_id.empty() || jpeg.empty()) return;
    CURL* curl = curl_easy_init();
    if (!curl) return;

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, chat_id.c_str(), CURL_ZERO_TERMINATED);
    if (!caption.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, caption.c_str(), CURL_ZERO_TERMINATED);
    }
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "photo");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_type(part, "image/jpeg");
    curl_mime_data(part, reinterpret_cast<const char*>(jpeg.data()), jpeg.size());

    std::string url = "https://api.telegram.org/bot" + bot_token_ + "/sendPhoto";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWrite);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::fprintf(stderr, "[텔레그램] 사진 전송 실패: %s\n", curl_easy_strerror(res));
    }
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
}

// ── getUpdates 롱폴링 ──
void TelegramModule::startPolling() {
    if (bot_token_.empty()) {
        std::fprintf(stderr, "[케어봇] bot_token 미설정 — 봇 폴링 비활성\n");
        return;
    }
    if (polling_.exchange(true)) return;  // 이미 켜짐
    poll_thread_ = std::thread([this]() { pollLoop(); });
}

void TelegramModule::stopPolling() {
    if (!polling_.exchange(false)) return;
    if (poll_thread_.joinable()) poll_thread_.join();
}

void TelegramModule::pollLoop() {
    std::fprintf(stderr, "[케어봇] getUpdates 폴링 시작\n");
    while (polling_) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        // 롱폴링: 새 업데이트가 없으면 서버가 최대 25초 붙잡았다 응답한다.
        std::string url = "https://api.telegram.org/bot" + bot_token_ +
                          "/getUpdates?timeout=25&offset=" +
                          std::to_string(update_offset_);
        std::string resp;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collectWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);  // 롱폴링 timeout보다 넉넉히

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            // 네트워크 순단 등 — 잠깐 쉬고 재시도 (폴링 스레드는 계속 산다)
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        try {
            auto j = nlohmann::json::parse(resp);
            if (!j.value("ok", false)) continue;
            for (const auto& upd : j.at("result")) {
                update_offset_ = upd.at("update_id").get<int64_t>() + 1;
                if (!upd.contains("message")) continue;
                const auto& msg = upd.at("message");
                if (!msg.contains("text") || !msg.contains("chat")) continue;

                std::string chat_id =
                    std::to_string(msg.at("chat").at("id").get<int64_t>());
                std::string text = msg.at("text").get<std::string>();

                if (!isAuthorized(chat_id)) continue;  // 미등록 사용자는 무시
                if (on_command_) {
                    on_command_(resolveChannel(chat_id), chat_id, text);
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[케어봇] getUpdates 파싱 실패: %s\n", e.what());
        }
    }
    std::fprintf(stderr, "[케어봇] 폴링 종료\n");
}
