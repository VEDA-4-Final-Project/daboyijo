#pragma once

#include <map>
#include <string>

// ══ [보호자 알림] 모듈 — 낙상/침상탈출 확정 시 텔레그램으로 메시지 전송 ══
//
// 봇은 전체 서버에 하나(bot_token)만 쓰고, 수신자(chat_id)는 채널별로 다르게 줄 수 있다.
// config/cameras.conf의 telegram_chat_id_<채널번호>로 채널별 보호자를 지정하고,
// 지정이 없는 채널은 telegram_chat_id(기본값)로 보낸다. bot_token이 비어 있거나
// 해당 채널에 쓸 chat_id가 하나도 없으면 조용히 무시 — 텔레그램 미설정 환경에서도
// 서버는 정상 동작한다.
class TelegramModule {
public:
    void configure(std::string bot_token, std::string default_chat_id,
                   std::map<int, std::string> chat_ids_by_channel);

    void notifyFall(int channel);
    void notifyEgress(int channel);

private:
    const std::string& resolveChatId(int channel) const;
    void sendAsync(int channel, std::string text) const;

    std::string bot_token_;
    std::string default_chat_id_;
    std::map<int, std::string> chat_ids_by_channel_;
};
