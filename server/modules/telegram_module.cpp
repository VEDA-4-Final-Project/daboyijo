#include "telegram_module.hpp"

#include <cstdio>
#include <thread>
#include <utility>

#include <curl/curl.h>

namespace {

// libcurl이 응답 본문을 받으면 호출 — 알림 전송에는 응답 내용이 필요 없어 버린다.
size_t discardWrite(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

}  // namespace

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

void TelegramModule::notifyFall(int channel) {
    sendAsync(channel, "🚨 [ch" + std::to_string(channel) + "] 낙상이 감지되었습니다. 확인 바랍니다.");
}

void TelegramModule::notifyEgress(int channel) {
    sendAsync(channel, "⚠️ [ch" + std::to_string(channel) + "] 침상 이탈이 감지되었습니다.");
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
