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

// "[ch1]" — 사람이 읽는 방 번호는 1부터다(내부 채널 인덱스는 0부터).
std::string TelegramModule::chLabel(int channel) {
    return "[ch" + std::to_string(channel + 1) + "]";
}

// "김○○ 님(침대2)" / 신원을 못 잡았으면 "신원 미상".
// ★ 이름을 붙이는 게 핵심이다. 한 방에 어르신이 두 분이면 "[ch1] 낙상"만으로는
//   보호자도 요양사도 누구인지 알 수 없어 알림이 사실상 무의미해진다.
std::string TelegramModule::subjectLabel(const Subject& who) {
    if (who.name.empty()) return "신원 미상";
    if (who.roi_id >= 0) {
        return who.name + " 님(침대" + std::to_string(who.roi_id + 1) + ")";
    }
    return who.name + " 님";
}

TelegramModule::~TelegramModule() { stopPolling(); }

void TelegramModule::configure(
    std::string bot_token, std::string default_chat_id,
    std::map<int, std::vector<std::string>> guardians_by_channel,
    std::map<int, std::string> guardians_by_resident,
    std::vector<std::string> staff_chat_ids, bool guardian_alerts) {
    bot_token_ = std::move(bot_token);
    default_chat_id_ = std::move(default_chat_id);
    guardians_by_channel_ = std::move(guardians_by_channel);
    guardians_by_resident_ = std::move(guardians_by_resident);
    staff_chat_ids_ = std::move(staff_chat_ids);
    guardian_alerts_ = guardian_alerts;
}

std::vector<std::string> TelegramModule::guardiansFor(int channel,
                                                      const Subject& who) const {
    // 1) 사람이 특정됐고 그 입소자의 보호자가 등록돼 있으면 그 한 명에게만.
    //    한 방에 어르신이 둘일 때 엉뚱한 보호자에게 가지 않게 하는 게 이 단계다.
    if (who.resident_id > 0) {
        auto rit = guardians_by_resident_.find(who.resident_id);
        if (rit != guardians_by_resident_.end() && !rit->second.empty()) {
            return {rit->second};
        }
    }
    // 2) 신원 미상이거나 입소자 매핑이 없으면 그 방 보호자 전원.
    //    누구 일인지 모를 때는 "둘 다에게"가 "엉뚱한 한 명에게만"보다 낫다.
    auto cit = guardians_by_channel_.find(channel);
    if (cit != guardians_by_channel_.end() && !cit->second.empty()) {
        return cit->second;
    }
    // 3) 채널 지정도 없으면 기본 수신자(단일 방 데모 등).
    if (!default_chat_id_.empty()) return {default_chat_id_};
    return {};
}

std::vector<TelegramModule::Recipient> TelegramModule::recipientsFor(
    int channel, const Subject& who) const {
    std::vector<Recipient> out;
    for (const auto& g : guardiansFor(channel, who)) {
        out.push_back({g, Role::Guardian});
    }
    for (const auto& st : staff_chat_ids_) {
        out.push_back({st, Role::Staff});
    }
    return out;
}

std::vector<int> TelegramModule::channelsForGuardian(
    const std::string& chat_id) const {
    std::vector<int> out;
    for (const auto& [ch, ids] : guardians_by_channel_) {
        for (const auto& id : ids) {
            if (id == chat_id) {
                out.push_back(ch);
                break;
            }
        }
    }
    return out;
}

std::optional<TelegramModule::Role> TelegramModule::roleFor(
    const std::string& chat_id) const {
    if (chat_id.empty()) return std::nullopt;
    // ★ 요양사를 먼저 본다 — 같은 계정이 양쪽에 등록됐다면(시연 중 흔하다) 권한이
    //   넓은 쪽으로 해석해야 방 선택 버튼이 나온다. 반대로 보면 ch0에 갇힌다.
    for (const auto& st : staff_chat_ids_) {
        if (st == chat_id) return Role::Staff;
    }
    if (!channelsForGuardian(chat_id).empty()) return Role::Guardian;
    for (const auto& [rid, id] : guardians_by_resident_) {
        (void)rid;
        if (id == chat_id) return Role::Guardian;
    }
    if (!default_chat_id_.empty() && chat_id == default_chat_id_) {
        return Role::Guardian;
    }
    return std::nullopt;  // 미등록 — 봇은 무시한다
}

void TelegramModule::warnUnknownChat(const std::string& chat_id) const {
    {
        std::lock_guard<std::mutex> lock(warn_mutex_);
        if (!warned_unknown_.insert(chat_id).second) return;  // 이미 알렸다
    }
    std::fprintf(stderr,
                 "[케어봇] 미등록 chat_id %s — 무시했습니다. cameras.conf 의 "
                 "telegram_chat_id_<채널> / telegram_staff_chat_id 에 넣으세요.\n"
                 "         (2-Pi 구성이라면 이 Pi 가 아니라 다른 Pi 설정일 수 "
                 "있습니다 — 같은 봇 토큰을 두 Pi 가 함께 폴링하면 메시지를 "
                 "번갈아 집어가 절반이 이렇게 버려집니다.)\n",
                 chat_id.c_str());
}

int TelegramModule::resolveChannel(const std::string& chat_id, Role role) const {
    if (role == Role::Staff) return -1;  // 전 채널 담당 — 버튼에서 방을 고른다
    const auto chs = channelsForGuardian(chat_id);
    return chs.size() == 1 ? chs.front() : -1;  // 여러 방이면 선택 버튼으로
}

void TelegramModule::notifyFall(int channel, const Subject& who) {
    // 낙상은 안전상 무음(🔕)이어도 반드시 전송한다.
    const std::string room = chLabel(channel);
    const std::string sub = subjectLabel(who);
    dispatch(channel, who,
             "🚨 " + room + " " + sub + " 낙상이 감지되었습니다. 확인 바랍니다.",
             "🚨 낙상 " + room + " " + sub + "\n즉시 확인 바랍니다.",
             /*respect_mute=*/false);
}

void TelegramModule::notifyEgress(int channel, const Subject& who) {
    // 침상 이탈은 무음 대상 — 보호자가 오늘 알림 끄기(🔕) 중이면 보호자만 빠진다.
    // 요양사는 무음 개념이 없어 그대로 받는다(근무 중 놓치면 안 되는 정보라서).
    const std::string room = chLabel(channel);
    const std::string sub = subjectLabel(who);
    dispatch(channel, who,
             "⚠️ " + room + " " + sub + " 침상 이탈이 감지되었습니다.",
             "⚠️ 침상 이탈 " + room + " " + sub,
             /*respect_mute=*/true);
}

void TelegramModule::notifyVitalAbnormal(int channel, const Subject& who) {
    // 생체데이터 이상은 낙상과 같이 건강에 직결되므로 무음이어도 반드시 전송한다.
    const std::string room = chLabel(channel);
    const std::string sub = subjectLabel(who);
    dispatch(channel, who,
             "🚨 " + room + " " + sub + " 생체데이터 이상이 감지되었습니다. 확인 바랍니다.",
             "🚨 생체 이상 " + room + " " + sub + "\n즉시 확인 바랍니다.",
             /*respect_mute=*/false);
}

bool TelegramModule::isMuted(const std::string& chat_id) const {
    std::lock_guard<std::mutex> lock(mute_mutex_);
    auto it = mute_until_.find(chat_id);
    return it != mute_until_.end() &&
           std::chrono::system_clock::now() < it->second;
}

bool TelegramModule::toggleMute(const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(mute_mutex_);
    auto it = mute_until_.find(chat_id);
    if (it != mute_until_.end() &&
        std::chrono::system_clock::now() < it->second) {
        mute_until_.erase(it);  // 무음 → 해제
        return false;
    }
    mute_until_[chat_id] = nextLocalMidnight();  // 오늘 자정까지 무음
    return true;
}

// 역할별로 다른 문면을 뿌린다. 보호자는 입소자→방 순으로 해석해 필요한 사람에게만,
// 요양사는 등록된 전원에게. 무음은 보호자에게만 적용된다.
void TelegramModule::dispatch(int channel, const Subject& who,
                              const std::string& guardian_text,
                              const std::string& staff_text,
                              bool respect_mute) const {
    if (bot_token_.empty()) return;  // 미설정 시 무시(데모 편의)

    // 보호자 알림을 끈 운영에서는 보호자 루프를 통째로 건너뛴다 — 보호자는 봇으로
    // 조회만 하고, 사건 push 는 요양사만 받는다.
    if (guardian_alerts_) {
        for (const auto& g : guardiansFor(channel, who)) {
            if (g.empty()) continue;
            // 🔕 오늘 알림 끄기 — 낙상·생체이상은 respect_mute=false로 들어와 예외.
            if (respect_mute && isMuted(g)) continue;
            sendAsyncTo(g, guardian_text);
        }
    }
    // ★ 요양사는 무음을 타지 않는다. 근무 중 알림을 끄는 경로 자체를 두지 않아서
    //   toggleMute 버튼도 보호자 메뉴에만 붙는다(care_qa::sendMenu 참고).
    for (const auto& st : staff_chat_ids_) {
        if (st.empty()) continue;
        sendAsyncTo(st, staff_text);
    }
}

void TelegramModule::sendAsyncTo(std::string chat_id, std::string text) const {
    if (bot_token_.empty() || chat_id.empty()) return;

    // RTSP/AI 워커 스레드에서 호출되므로, HTTPS 왕복 지연이 실시간 파이프라인을
    // 막지 않도록 별도 스레드에서 전송하고 결과를 기다리지 않는다.
    std::thread([token = bot_token_, chat_id = std::move(chat_id),
                 text = std::move(text)]() {
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
    // 기동 로그에 등록 현황을 남긴다 — "설정을 고쳤는데 반영이 됐나"를 서버를
    // 띄우는 순간 확인할 수 있어야 한다(예전 바이너리로 돌고 있는 사고가 흔하다).
    std::fprintf(stderr,
                 "[케어봇] 수신자 등록: 보호자 채널 %zu개, 입소자 매핑 %zu개, "
                 "요양사 %zu명, 보호자 알림 %s\n",
                 guardians_by_channel_.size(), guardians_by_resident_.size(),
                 staff_chat_ids_.size(), guardian_alerts_ ? "ON" : "OFF");

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
                    const auto role = roleFor(chat_id);
                    if (!role) {
                        warnUnknownChat(chat_id);
                        continue;
                    }
                    if (on_callback_) {
                        on_callback_(resolveChannel(chat_id, *role), chat_id,
                                     *role, data);
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

                const auto role = roleFor(chat_id);
                if (!role) {
                    warnUnknownChat(chat_id);
                    continue;
                }
                if (on_command_) {
                    on_command_(resolveChannel(chat_id, *role), chat_id, *role,
                                text);
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[케어봇] getUpdates 파싱 실패: %s\n", e.what());
        }
    }
    std::fprintf(stderr, "[케어봇] 폴링 종료\n");
}
