#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

// ══ [보호자 알림 + 케어 봇] 텔레그램 모듈 ══
//
// (1) 단방향 알림: 낙상/침상탈출 확정 시 채널별 보호자에게 메시지 전송.
// (2) 양방향 봇: getUpdates 롱폴링으로 보호자 질문을 받아 커맨드 핸들러로 넘긴다
//     ("엄마 지금 뭐해?" → 케어 QA 모듈이 스냅샷+VLM으로 답). 인증은 config에
//     등록된 chat_id만 허용하고, chat_id로 어느 방(채널)인지 역매핑한다.
//
// 봇은 전체 서버에 하나(bot_token)만 쓰고, 수신자(chat_id)는 채널별로 다르게 준다.
// telegram_chat_id_<채널번호>로 채널별 보호자를 지정하고, 지정이 없는 채널은
// telegram_chat_id(기본값)로 보낸다. bot_token이 비어 있으면 전송/폴링 모두
// 조용히 비활성 — 텔레그램 미설정 환경에서도 서버는 정상 동작한다.
class TelegramModule {
public:
    // 보호자 질문 1건 → (channel, chatId, text). channel<0이면 방 매핑 불가.
    using CommandHandler =
        std::function<void(int channel, const std::string& chatId,
                           const std::string& text)>;

    ~TelegramModule();

    void configure(std::string bot_token, std::string default_chat_id,
                   std::map<int, std::string> chat_ids_by_channel);

    // ── 단방향 알림 (AI 워커 스레드에서 호출, 비동기 전송) ──
    void notifyFall(int channel);
    void notifyEgress(int channel);

    // ── 양방향 봇 ──
    void setCommandHandler(CommandHandler h) { on_command_ = std::move(h); }
    void startPolling();  // getUpdates 롱폴링 스레드 기동 (bot_token 없으면 무시)
    void stopPolling();   // 폴링 중단 + 스레드 join

    // 특정 chat_id로 직접 회신 (봇 응답용, 동기 전송 — 호출자 스레드에서 왕복).
    void sendMessage(const std::string& chat_id, const std::string& text) const;
    void sendPhoto(const std::string& chat_id,
                   const std::vector<unsigned char>& jpeg,
                   const std::string& caption) const;

    // 채널 → 수신자 chat_id (채널별 지정 없으면 기본값). 없으면 빈 문자열.
    // 낙상 자동 리포트 등 "채널을 아는 쪽"이 보낼 때 쓴다.
    std::string chatIdForChannel(int channel) const { return resolveChatId(channel); }

private:
    const std::string& resolveChatId(int channel) const;
    void sendAsync(int channel, std::string text) const;  // 알림용 비동기 전송

    // chat_id로 채널 역매핑 (없으면 -1). 인증도 이걸로 (등록 안 됐으면 무시).
    int resolveChannel(const std::string& chat_id) const;
    bool isAuthorized(const std::string& chat_id) const;
    void pollLoop();

    std::string bot_token_;
    std::string default_chat_id_;
    std::map<int, std::string> chat_ids_by_channel_;

    CommandHandler on_command_;
    std::thread poll_thread_;
    std::atomic<bool> polling_{false};
    int64_t update_offset_ = 0;  // getUpdates offset (처리한 update_id+1)
};
