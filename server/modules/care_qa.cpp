#include "care_qa.hpp"

#include <string>
#include <thread>

namespace {

constexpr int kKeyframes = 3;       // VLM에 보낼 키프레임 수
constexpr double kSpanSec = 5.0;    // 최근 몇 초 구간에서 뽑을지
constexpr double kReportCooldownSec = 60.0;  // 낙상 자동 리포트 채널당 최소 간격

// 앞의 공백 제거 + 소문자화 없이 명령 접두 판별용 간단 헬퍼
bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

const char* kHelp =
    "🤖 케어봇 사용법\n"
    "· 그냥 물어보세요: \"엄마 지금 뭐해?\" → 지금 상황을 알려드려요\n"
    "· /확인 : 낙상 경보를 확인 처리합니다\n"
    "· /도움 : 이 안내를 다시 봅니다";

}  // namespace

void CareQaModule::handleMessage(int channel, const std::string& chat_id,
                                 const std::string& text) {
    // ── 명령 처리 ──
    if (startsWith(text, "/확인") || startsWith(text, "/confirm")) {
        if (channel < 0) {
            telegram_.sendMessage(chat_id,
                "어느 방인지 알 수 없어 확인 처리를 못 했어요. 관리자에게 문의해 주세요.");
            return;
        }
        if (on_confirm_) on_confirm_(channel);
        telegram_.sendMessage(chat_id, "✅ 낙상 경보를 확인 처리했어요.");
        return;
    }
    if (startsWith(text, "/도움") || startsWith(text, "/help") ||
        startsWith(text, "/start")) {
        telegram_.sendMessage(chat_id, kHelp);
        return;
    }

    // ── 자연어 질문 → VLM 상황 설명 ──
    if (channel < 0) {
        telegram_.sendMessage(chat_id,
            "보호자 계정에 방(채널)이 연결돼 있지 않아요. 관리자에게 "
            "telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
        return;
    }
    if (!vlm_.available()) {
        telegram_.sendMessage(chat_id,
            "지금은 상황 확인 기능이 설정돼 있지 않아요. (관리자 설정 필요)");
        return;
    }

    // 수 초 걸리는 VLM 왕복은 폴링 루프를 막지 않도록 별도 스레드에서.
    SnapshotBuffer* snap = &snapshots_;
    VlmClient* vlm = &vlm_;
    TelegramModule* tg = &telegram_;
    std::thread([snap, vlm, tg, channel, chat_id, text]() {
        auto frames = snap->recentKeyframes(channel, kKeyframes, kSpanSec);
        if (frames.empty()) {
            tg->sendMessage(chat_id,
                "지금 영상이 들어오지 않아 상황을 확인하기 어려워요.");
            return;
        }
        std::string answer = vlm->describe(frames, text);
        if (answer.empty()) {
            tg->sendMessage(chat_id,
                "일시적으로 상황을 확인하기 어려워요. 잠시 후 다시 시도해 주세요.");
            return;
        }
        tg->sendMessage(chat_id, answer);
        tg->sendPhoto(chat_id, frames.back(), "지금 상황 📷");
    }).detach();
}

void CareQaModule::reportFall(int channel) {
    if (!vlm_.available()) return;  // VLM 미설정이면 기본 알림(notifyFall)만
    std::string chat_id = telegram_.chatIdForChannel(channel);
    if (chat_id.empty()) return;

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
    SnapshotBuffer* snap = &snapshots_;
    VlmClient* vlm = &vlm_;
    TelegramModule* tg = &telegram_;
    std::thread([snap, vlm, tg, channel, chat_id]() {
        auto frames = snap->recentKeyframes(channel, kKeyframes, kSpanSec);
        if (frames.empty()) return;
        const std::string q =
            "방금 이 어르신에게 낙상이 감지되었습니다. 사진 속 현재 자세와 "
            "상황을 보호자에게 알리듯 두세 문장으로 설명해 주세요. 의학적 진단이나 "
            "부상 여부 단정은 하지 말고, 보이는 상황만 차분히 전해 주세요.";
        std::string answer = vlm->describe(frames, q);
        if (answer.empty()) return;
        tg->sendMessage(chat_id, "🚨 낙상 감지 — 현재 상황\n" + answer);
        tg->sendPhoto(chat_id, frames.back(), "낙상 감지 시점 📷");
    }).detach();
}
