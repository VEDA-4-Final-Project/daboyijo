#include "telegram_module.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
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

// 다음 로컬 자정(오늘 23:59:59 다음 순간). "오늘 하루만 무음"의 만료 시각으로 쓴다.
std::chrono::system_clock::time_point nextLocalMidnight() {
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday += 1;     // 내일 00:00
    tm.tm_isdst = -1;    // DST는 mktime이 알아서
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
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
    // 낙상은 안전상 무음(🔕)이어도 반드시 전송한다.
    sendAsync(channel, "🚨 [ch" + std::to_string(channel + 1) + "] 낙상이 감지되었습니다. 확인 바랍니다.",
              /*respect_mute=*/false);
}

void TelegramModule::notifyEgress(int channel) {
    // 침상 이탈은 무음 대상 — 오늘 알림 끄기(🔕) 중이면 전송 생략.
    sendAsync(channel, "⚠️ [ch" + std::to_string(channel + 1) + "] 침상 이탈이 감지되었습니다.",
              /*respect_mute=*/true);
}

void TelegramModule::notifyVitalAbnormal(int channel) {
    // 생체데이터 이상은 낙상과 같이 건강에 직결되므로 무음이어도 반드시 전송한다.
    sendAsync(channel, "🚨 [ch" + std::to_string(channel + 1) + "] 생체데이터 이상이 감지되었습니다. 확인 바랍니다.",
              /*respect_mute=*/false);
}

bool TelegramModule::isMuted(int channel) const {
    std::lock_guard<std::mutex> lock(mute_mutex_);
    auto it = mute_until_.find(channel);
    return it != mute_until_.end() &&
           std::chrono::system_clock::now() < it->second;
}

bool TelegramModule::toggleMute(int channel) {
    std::lock_guard<std::mutex> lock(mute_mutex_);
    auto it = mute_until_.find(channel);
    if (it != mute_until_.end() &&
        std::chrono::system_clock::now() < it->second) {
        mute_until_.erase(it);  // 무음 → 해제
        return false;
    }
    mute_until_[channel] = nextLocalMidnight();  // 오늘 자정까지 무음
    return true;
}

void TelegramModule::sendAsync(int channel, std::string text, bool respect_mute) const {
    const std::string& chat_id_ref = resolveChatId(channel);
    if (bot_token_.empty() || chat_id_ref.empty()) return;  // 미설정 시 무시(데모 편의)
    if (respect_mute && isMuted(channel)) return;  // 🔕 오늘 알림 끄기 — 낙상은 예외(항상 전송)

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
            // LOGOFF std::fprintf(stderr, "[텔레그램] 전송 실패: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }).detach();
}

// ── 봇 응답: 특정 chat_id로 직접 전송 (동기, 호출자 스레드에서 왕복) ──
// reply_markup(인라인 키보드 JSON)이 있으면 버튼도 함께 붙인다.
void TelegramModule::sendMessage(const std::string& chat_id,
                                 const std::string& text,
                                 const std::string& reply_markup) const {
    if (bot_token_.empty() || chat_id.empty()) return;
    CURL* curl = curl_easy_init();
    if (!curl) return;

    char* chat_id_esc = curl_easy_escape(curl, chat_id.c_str(), 0);
    char* text_esc = curl_easy_escape(curl, text.c_str(), 0);
    std::string fields = "chat_id=" + std::string(chat_id_esc) + "&text=" + std::string(text_esc);
    curl_free(chat_id_esc);
    curl_free(text_esc);
    if (!reply_markup.empty()) {
        char* rm_esc = curl_easy_escape(curl, reply_markup.c_str(), 0);
        fields += "&reply_markup=" + std::string(rm_esc);
        curl_free(rm_esc);
    }

    std::string url = "https://api.telegram.org/bot" + bot_token_ + "/sendMessage";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWrite);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
}

// ── 버튼 클릭 확인(answerCallbackQuery) — 누른 버튼의 로딩 스피너를 꺼준다 ──
void TelegramModule::answerCallbackQuery(const std::string& callback_query_id) const {
    if (bot_token_.empty() || callback_query_id.empty()) return;
    CURL* curl = curl_easy_init();
    if (!curl) return;

    char* id_esc = curl_easy_escape(curl, callback_query_id.c_str(), 0);
    std::string fields = "callback_query_id=" + std::string(id_esc);
    curl_free(id_esc);

    std::string url = "https://api.telegram.org/bot" + bot_token_ + "/answerCallbackQuery";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWrite);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
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
        // LOGOFF std::fprintf(stderr, "[텔레그램] 사진 전송 실패: %s\n", curl_easy_strerror(res));
    }
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
}

// ── getUpdates 롱폴링 ──
void TelegramModule::startPolling() {
    if (bot_token_.empty()) {
        // LOGOFF std::fprintf(stderr, "[케어봇] bot_token 미설정 — 봇 폴링 비활성\n");
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
    // LOGOFF std::fprintf(stderr, "[케어봇] getUpdates 폴링 시작\n");
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

                // ── 인라인 버튼 클릭 ──
                if (upd.contains("callback_query")) {
                    const auto& cq = upd.at("callback_query");
                    // 어느 버튼이든 스피너부터 끈다(응답 여부와 무관하게).
                    if (cq.contains("id")) {
                        answerCallbackQuery(cq.at("id").get<std::string>());
                    }
                    if (!cq.contains("data")) continue;
                    // 회신은 버튼이 눌린 채팅으로 — 그룹에서 쓰려면 from.id(누른
                    // 사람 개인)가 아니라 message.chat.id(그 그룹)를 써야 알림·회신이
                    // 등록된 그룹 채팅으로 간다. 메시지가 너무 오래돼 message가 빠진
                    // 경우에만 from.id로 폴백.
                    std::string chat_id;
                    if (cq.contains("message") &&
                        cq.at("message").contains("chat")) {
                        chat_id = std::to_string(
                            cq.at("message").at("chat").at("id").get<int64_t>());
                    } else if (cq.contains("from")) {
                        chat_id = std::to_string(
                            cq.at("from").at("id").get<int64_t>());
                    }
                    if (chat_id.empty()) continue;
                    std::string data = cq.at("data").get<std::string>();
                    if (!isAuthorized(chat_id)) continue;  // 미등록 채팅은 무시
                    if (on_callback_) {
                        on_callback_(resolveChannel(chat_id), chat_id, data);
                    }
                    continue;
                }

                // ── 일반 텍스트 메시지 → 메뉴 표시 ──
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
            // LOGOFF std::fprintf(stderr, "[케어봇] getUpdates 파싱 실패: %s\n", e.what());
        }
    }
    // LOGOFF std::fprintf(stderr, "[케어봇] 폴링 종료\n");
}
