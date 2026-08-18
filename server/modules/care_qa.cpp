#include "care_qa.hpp"

#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

constexpr int kKeyframes = 3;       // VLM에 보낼 키프레임 수
constexpr double kSpanSec = 5.0;    // 최근 몇 초 구간에서 뽑을지
constexpr double kReportCooldownSec = 60.0;  // 낙상 자동 리포트 채널당 최소 간격

const char* kHelp =
    "🤖 케어봇 사용법\n"
    "· 아무 메시지나 보내면 버튼 메뉴가 나와요.\n"
    "· 📷 지금 상황 보기 : 방 상황을 사진과 함께 알려드려요\n"
    "· 🔍 영상 검색 : \"어제 저녁에 낙상 있었어?\"처럼 물어보면 지난 기록을 찾아드려요\n"
    "· 📞 연락처 : 요양사·관리자 연락처\n"
    "· 🔕/🔔 알림 : 오늘 이 방 침상 이탈 알림을 껐다 켰다 (자정 자동 해제)\n"
    "  ※ 낙상 알림은 안전상 무음과 무관하게 항상 전송돼요.";

// 📷 지금 상황 보기 버튼이 VLM에 던지는 고정 프롬프트.
// (보호자 원문을 그대로 넘기던 방식과 달리, 용도를 요양 상황 설명으로 못박아
//  엉뚱한 질문·프롬프트 인젝션을 원천 차단한다.)
const char* kNowQuestion =
    "요양원 실내 CCTV 사진입니다(사생활 보호로 얼굴은 블러 처리됨). 화면 속 "
    "어르신이 지금 무엇을 하고 계신지(앉아 계심/누워 계심/걷는 중/식사 중 등)와 "
    "특이사항이 있는지 보호자에게 알리듯 두세 문장으로 차분하고 다정하게 알려주세요. "
    "의학적 진단은 하지 말고 보이는 상황만 설명하세요.";

}  // namespace

// 기본은 내용과 무관하게 버튼 메뉴를 띄운다 — 단, 🔍 영상 검색 버튼을 누른
// 직후의 chat_id는 예외로, 이번 메시지를 검색 질의로 소비한다(1회성).
void CareQaModule::handleMessage(int channel, const std::string& chat_id,
                                 const std::string& text) {
    bool awaitingSearch = false;
    {
        std::lock_guard<std::mutex> lock(search_mutex_);
        auto it = awaiting_search_.find(chat_id);
        if (it != awaiting_search_.end()) {
            awaiting_search_.erase(it);
            awaitingSearch = true;
        }
    }
    if (awaitingSearch) {
        if (channel < 0) {
            telegram_.sendMessage(chat_id,
                "보호자 계정에 방(채널)이 연결돼 있지 않아 검색할 수 없어요. "
                "관리자에게 telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
            sendMenu(channel, chat_id);
            return;
        }
        // Gemini+DB 왕복은 폴링 루프를 막지 않도록 detached 스레드에서(now와 동일 패턴).
        std::thread([this, channel, chat_id, text]() {
            const std::string answer = video_search_.search(channel, text);
            telegram_.sendMessage(chat_id, answer);
            sendMenu(channel, chat_id);
        }).detach();
        return;
    }

    (void)text;  // 검색 대기 중이 아니면 내용은 쓰지 않는다 — 뭘 쓰든 메뉴로 안내
    sendMenu(channel, chat_id);
}

void CareQaModule::sendMenu(int channel, const std::string& chat_id) {
    // 🔕/🔔 라벨은 현재 무음 상태에 맞춰 바뀐다.
    const bool muted = channel >= 0 && telegram_.isMuted(channel);
    const char* mute_label = muted ? "🔔 알림 다시 켜기" : "🔕 오늘 알림 끄기";

    // 인라인 키보드 JSON을 명시적으로 조립한다(nlohmann 중첩 브레이스는 단일
    // 요소 행에서 배열/객체를 오판할 수 있어 push_back으로 확실히 배열화).
    auto button = [](const std::string& text, const std::string& data) {
        nlohmann::json b;
        b["text"] = text;
        b["callback_data"] = data;
        return b;
    };
    nlohmann::json row_now = nlohmann::json::array();
    row_now.push_back(button("📷 지금 상황 보기", "now"));
    row_now.push_back(button("🔍 영상 검색", "search"));
    nlohmann::json row_info = nlohmann::json::array();
    row_info.push_back(button("📞 연락처", "contact"));
    row_info.push_back(button("ℹ️ 도움말", "help"));
    nlohmann::json row_mute = nlohmann::json::array();
    row_mute.push_back(button(mute_label, "mute"));

    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(row_now);
    rows.push_back(row_info);
    rows.push_back(row_mute);

    nlohmann::json kb;
    kb["inline_keyboard"] = rows;

    telegram_.sendMessage(chat_id, "무엇을 도와드릴까요?", kb.dump());
}

void CareQaModule::handleCallback(int channel, const std::string& chat_id,
                                  const std::string& data) {
    // ── 📞 연락처 ──
    if (data == "contact") {
        const std::string caregiver =
            contact_caregiver_.empty() ? "미등록" : contact_caregiver_;
        const std::string manager =
            contact_manager_.empty() ? "미등록" : contact_manager_;
        telegram_.sendMessage(chat_id,
            "📞 연락처\n· 요양사: " + caregiver + "\n· 관리자: " + manager);
        sendMenu(channel, chat_id);  // 대답 후 메뉴 다시 표시
        return;
    }

    // ── ℹ️ 도움말 ──
    if (data == "help") {
        telegram_.sendMessage(chat_id, kHelp);
        sendMenu(channel, chat_id);  // 대답 후 메뉴 다시 표시
        return;
    }

    // ── 🔕/🔔 알림 토글 ──
    if (data == "mute") {
        if (channel < 0) {
            telegram_.sendMessage(chat_id,
                "보호자 계정에 방(채널)이 연결돼 있지 않아 알림 설정을 바꿀 수 "
                "없어요. 관리자에게 telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
            sendMenu(channel, chat_id);
            return;
        }
        const bool now_muted = telegram_.toggleMute(channel);
        telegram_.sendMessage(chat_id,
            now_muted ? "🔕 오늘 이 방 침상 이탈 알림을 껐어요. 자정에 자동으로 다시 "
                        "켜집니다.\n(낙상 알림은 안전상 항상 전송됩니다.)"
                      : "🔔 이 방 침상 이탈 알림을 다시 켰어요.");
        sendMenu(channel, chat_id);  // 토글 후 메뉴 재표시(🔕/🔔 라벨도 갱신됨)
        return;
    }

    // ── 📷 지금 상황 보기 ──
    if (data == "now") {
        if (channel < 0) {
            telegram_.sendMessage(chat_id,
                "보호자 계정에 방(채널)이 연결돼 있지 않아요. 관리자에게 "
                "telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
            sendMenu(channel, chat_id);
            return;
        }
        if (!vlm_.available()) {
            telegram_.sendMessage(chat_id,
                "지금은 상황 확인 기능이 설정돼 있지 않아요. (관리자 설정 필요)");
            sendMenu(channel, chat_id);
            return;
        }
        // 수 초 걸리는 VLM 왕복은 폴링 루프를 막지 않도록 별도 스레드에서.
        // VLM 답변 전송이 끝난 뒤 메뉴를 다시 띄우려면 this가 필요하다(sendMenu).
        std::thread([this, channel, chat_id]() {
            auto frames = snapshots_.recentKeyframes(channel, kKeyframes, kSpanSec);
            if (frames.empty()) {
                telegram_.sendMessage(chat_id,
                    "지금 영상이 들어오지 않아 상황을 확인하기 어려워요.");
                sendMenu(channel, chat_id);
                return;
            }
            std::string answer = vlm_.describe(frames, kNowQuestion);
            if (answer.empty()) {
                telegram_.sendMessage(chat_id,
                    "일시적으로 상황을 확인하기 어려워요. 잠시 후 다시 시도해 주세요.");
                sendMenu(channel, chat_id);
                return;
            }
            telegram_.sendMessage(chat_id, answer);
            telegram_.sendPhoto(chat_id, frames.back(), "지금 상황 📷");
            sendMenu(channel, chat_id);  // 상황 설명·사진 전송 후 메뉴 다시 표시
        }).detach();
        return;
    }

    // ── 🔍 영상 검색 ──
    if (data == "search") {
        if (channel < 0) {
            telegram_.sendMessage(chat_id,
                "보호자 계정에 방(채널)이 연결돼 있지 않아요. 관리자에게 "
                "telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
            sendMenu(channel, chat_id);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(search_mutex_);
            awaiting_search_.insert(chat_id);
        }
        telegram_.sendMessage(chat_id,
            "🔎 찾으시는 상황을 말씀해 주세요.\n"
            "예: \"어제 저녁에 낙상 있었어?\", \"오늘 오전에 침대에서 나간 적 있어?\"");
        // 메뉴는 다시 안 띄운다 — 지금은 다음 메시지를 검색어로 기다리는 중.
        return;
    }

    // 알 수 없는 버튼 — 메뉴 다시 표시
    sendMenu(channel, chat_id);
}

void CareQaModule::reportFall(int channel) {
    std::string chat_id = telegram_.chatIdForChannel(channel);
    if (chat_id.empty()) return;
    // 낙상 리포트(사진+VLM 설명)는 안전상 무음(🔕)이어도 항상 전송한다.

    // 쿨다운: 지속 낙상으로 콜백이 반복돼도 채널당 kReportCooldownSec 이내엔 1회만
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto it = last_report_.find(channel);
        if (it != last_report_.end() &&
            std::chrono::duration<double>(now - it->second).count() <
                kReportCooldownSec) {
            return;
        }
        last_report_[channel] = now;
    }

    // VLM 왕복(수 초)은 fall 콜백(AI 워커 스레드)을 막지 않도록 detached 스레드에서.
    SnapshotBuffer* snapA = &snapshots_;       // 전원 블러본 (Gemini용)
    SnapshotBuffer* snapB = &snapshots_fall_;  // 낙상 선택본 (보호자 사진용)
    VlmClient* vlm = &vlm_;
    TelegramModule* tg = &telegram_;
    std::thread([snapA, snapB, vlm, tg, channel, chat_id]() {
        // ── 1) 낙상 시점 사진 먼저 확보·전송 ──
        // 사진 fetch가 Gemini 왕복(최대 20초) 뒤에 있으면 "낙상 한참 뒤" 장면이
        // 날아간다. 그래서 설명보다 먼저, 낙상 직후 프레임을 붙잡아 보낸다.
        // 선택본(버퍼 B)은 privacy_masker.reportFall 직후부터 채워지므로 낙상
        // 확정 순간엔 아직 비어 있을 수 있다 — 짧게(최대 ~1.5초) 폴링해 "첫
        // 선택본"(=낙상 시점 프레임)을 잡고, 그래도 없으면 전원 블러본으로 폴백.
        SnapshotBuffer::Jpeg shot;
        for (int i = 0; i < 15; ++i) {
            auto s = snapB->recentKeyframes(channel, 1, kSpanSec);
            if (!s.empty()) {
                shot = std::move(s.back());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (shot.empty()) {  // 선택본이 안 잡히면 전원 블러본 최신 1장으로 폴백
            auto b = snapA->recentKeyframes(channel, 1, kSpanSec);
            if (!b.empty()) shot = std::move(b.back());
        }
        if (!shot.empty()) tg->sendPhoto(chat_id, shot, "낙상 감지 시점 📷");

        // ── 2) Gemini 상황 설명 ── 반드시 전원 블러본(버퍼 A)만 외부로.
        // 사진은 이미 나갔으므로, 이 왕복이 느려도 사진 타이밍엔 영향 없다.
        if (vlm->available()) {
            auto blurred = snapA->recentKeyframes(channel, kKeyframes, kSpanSec);
            if (!blurred.empty()) {
                const std::string q =
                    "요양원 실내 CCTV에서 낙상 감지 시스템이 방금 낙상을 감지한 직후의 "
                    "사진들입니다(촬영 순서, 사생활 보호로 얼굴은 블러 처리됨). 화면 속 "
                    "어르신이 바닥이나 매트 위에 누워·엎드려·주저앉아 계시면, 이는 침상에서의 "
                    "휴식이 아니라 넘어져 쓰러진 '낙상' 상황입니다. 어르신의 현재 자세(옆으로 "
                    "누움/엎드림/뒤로 넘어짐 등)와 움직임 여부, 손·다리 위치를 보호자에게 "
                    "알리듯 두세 문장으로 차분하고 다정하게 전해 주세요. 의학적 진단이나 부상 "
                    "여부는 단정하지 말고, 보이는 상황만 설명하세요.";
                std::string answer = vlm->describe(blurred, q);
                if (!answer.empty()) {
                    tg->sendMessage(chat_id, "🚨 낙상 감지 — 현재 상황\n" + answer);
                }
            }
        }
    }).detach();
}
