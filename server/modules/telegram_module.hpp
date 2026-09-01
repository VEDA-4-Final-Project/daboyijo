#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ══ [보호자 + 요양사 알림 / 케어 봇] 텔레그램 모듈 ══
//
// (1) 단방향 알림: 낙상/침상탈출/생체이상 확정 시 수신자에게 전송.
// (2) 양방향 봇: getUpdates 롱폴링으로 질문을 받아 커맨드 핸들러로 넘긴다
//     ("지금 상황 보기" → 케어 QA 모듈이 스냅샷+VLM으로 답). 인증은 config에
//     등록된 chat_id만 허용한다.
//
// ── 수신자는 역할(Role)로 갈린다 ────────────────────────────────
// 봇은 서버 전체에 하나(bot_token)만 쓰고, 누가 무엇을 받는지는 chat_id의 역할로
// 정한다. 봇을 둘로 쪼개지 않은 이유: 폴링 스레드가 2개가 되면 2-Pi 구성에서
// getUpdates 큐를 나눠 먹는 문제가 그대로 2배가 된다.
//
//   Guardian(보호자) — 자기 어르신 일만 받는다. 무음(🔕) 적용 대상.
//   Staff(요양사)    — 전 채널을 본다. 무음이 없어 알림이 항상 간다.
//
// 보호자는 두 층으로 잡는다. 한 방에 어르신이 두 분 누우면 "방을 볼 권한"과
// "이 사고를 받을 사람"이 갈리기 때문이다.
//   telegram_chat_id_<채널>              : 그 방을 조회할 수 있는 보호자들(콤마로 여럿)
//   telegram_guardian_chat_id_<입소자id> : 그 입소자 사고를 받을 보호자 1명
// 사람이 특정된 사건은 입소자 매핑을 먼저 쓰고, 신원 미상이면 그 방 보호자
// 전원으로 폴백한다 — 방에 2명인데 매핑이 없을 때 "둘 다에게"가 "엉뚱한 한
// 명에게만"보다 낫기 때문.
//
// bot_token이 비어 있으면 전송/폴링 모두 조용히 비활성 — 텔레그램 미설정
// 환경에서도 서버는 정상 동작한다.
class TelegramModule {
public:
    enum class Role { Guardian, Staff };

    // 사건의 당사자. 낙상 콜백은 IdentityTracker로 이미 이 값들을 알고 있다.
    // resident_id==0 / name 비어있음 = 신원 미상(그 방 보호자 전원에게 폴백).
    struct Subject {
        int resident_id = 0;
        std::string name;
        int roi_id = -1;  // 침대 인덱스(0-based). -1이면 침대 미상.
    };

    struct Recipient {
        std::string chat_id;
        Role role;
    };

    // 메시지 1건 → (channel, chatId, role, text).
    // channel<0이면 방을 하나로 특정할 수 없다는 뜻 — 요양사(전 채널)거나,
    // 여러 방에 걸린 보호자다. 이 경우 케어봇이 방 선택 버튼을 띄운다.
    using CommandHandler =
        std::function<void(int channel, const std::string& chatId, Role role,
                           const std::string& text)>;

    // 인라인 버튼 클릭 1건 → (channel, chatId, role, data). data는 callback_data.
    using CallbackHandler =
        std::function<void(int channel, const std::string& chatId, Role role,
                           const std::string& data)>;

    ~TelegramModule();

    void configure(std::string bot_token, std::string default_chat_id,
                   std::map<int, std::vector<std::string>> guardians_by_channel,
                   std::map<int, std::string> guardians_by_resident,
                   std::vector<std::string> staff_chat_ids,
                   bool guardian_alerts);

    // 보호자에게 사건 알림을 보내는 설정인지. 꺼져 있으면 케어봇이 보호자 메뉴에서
    // 🔕 무음 버튼을 빼고(끌 알림이 없다), 알림은 요양사에게만 나간다.
    bool guardianAlertsEnabled() const { return guardian_alerts_; }

    // ── 단방향 알림 (AI 워커 스레드에서 호출, 비동기 전송) ──
    // 보호자와 요양사에게 각각 다른 문면이 나간다 — 보호자는 안심시키는 톤,
    // 요양사는 조치 판단에 필요한 사실(방·침대·이름) 위주.
    void notifyFall(int channel, const Subject& who);
    void notifyEgress(int channel, const Subject& who);
    void notifyVitalAbnormal(int channel, const Subject& who);

    // ── 양방향 봇 ──
    void setCommandHandler(CommandHandler h) { on_command_ = std::move(h); }
    void setCallbackHandler(CallbackHandler h) { on_callback_ = std::move(h); }
    void startPolling();  // getUpdates 롱폴링 스레드 기동 (bot_token 없으면 무시)
    void stopPolling();   // 폴링 중단 + 스레드 join

    // ── 알림 무음 (🔕/🔔 토글 버튼) ──
    // ★ 채널이 아니라 chat_id 단위다. 한 방에 보호자가 둘일 수 있어 채널로 걸면
    //   한 사람이 끈 무음이 다른 보호자 알림까지 죽인다. 요양사는 이 버튼 자체를
    //   받지 않으므로 무음 상태를 가질 일이 없다.
    // 무음은 "오늘 하루"만 — 로컬 자정에 자동 해제된다(무음 걸고 잊는 사고 방지).
    bool toggleMute(const std::string& chat_id);       // 반환 true=이제 무음
    bool isMuted(const std::string& chat_id) const;    // 버튼 라벨 판정에 사용

    // 특정 chat_id로 직접 회신 (봇 응답용, 동기 전송 — 호출자 스레드에서 왕복).
    // reply_markup(인라인 키보드 JSON)이 있으면 함께 보낸다.
    void sendMessage(const std::string& chat_id, const std::string& text,
                     const std::string& reply_markup = std::string()) const;
    void sendPhoto(const std::string& chat_id,
                   const std::vector<unsigned char>& jpeg,
                   const std::string& caption) const;

    // 이 사건을 받아야 할 사람 전원(보호자 + 요양사 전원).
    // 낙상 자동 리포트처럼 "한 번 만들어 여러 명에게 뿌리는" 쪽에서 쓴다.
    // ★ VLM 호출은 이 목록을 돌기 전에 한 번만 해야 한다 — 사람 수만큼 부르면
    //   같은 사진을 두 번 태워 무료 티어 일일 한도가 그만큼 빨리 마른다.
    std::vector<Recipient> recipientsFor(int channel, const Subject& who) const;

    // 이 보호자가 볼 수 있는 방들. 1개면 자동 선택, 여러 개면 선택 버튼을 띄운다.
    std::vector<int> channelsForGuardian(const std::string& chat_id) const;

    // 등록된 chat_id면 역할을, 아니면 nullopt(= 미인증).
    std::optional<Role> roleFor(const std::string& chat_id) const;

    // 사람이 읽는 라벨. 알림 문면과 케어봇 회신이 같은 표기를 쓰도록 한 곳에서만
    // 만든다 — 따로 쓰면 알림은 "[ch1] 김○○ 님(침대2)", 봇은 "1번 방"처럼 갈린다.
    static std::string chLabel(int channel);              // "[ch1]"
    static std::string subjectLabel(const Subject& who);  // "김○○ 님(침대2)"

private:
    // 보호자 1명 해석: 입소자 매핑 우선 → 없으면 그 방 보호자 전원 → 그래도
    // 없으면 기본 chat_id. 반환이 비면 보낼 보호자가 없다는 뜻.
    std::vector<std::string> guardiansFor(int channel, const Subject& who) const;

    // 역할별로 다른 문면을 뿌린다. 보호자는 respect_mute를 따르고, 요양사는
    // 무음과 무관하게 항상 받는다.
    void dispatch(int channel, const Subject& who,
                  const std::string& guardian_text,
                  const std::string& staff_text, bool respect_mute) const;
    void sendAsyncTo(std::string chat_id, std::string text) const;

    // 보호자 chat_id → 그 방 번호(여러 방이면 -1). 요양사는 항상 -1.
    int resolveChannel(const std::string& chat_id, Role role) const;
    // 미등록 chat_id 를 처음 봤을 때만 경고한다. 조용히 버리면 "왜 이 방만 답이
    // 없지"를 설정 파일만 노려보며 찾게 되고, 매번 찍으면 남이 봇에 말을 걸 때마다
    // 로그가 밀린다.
    void warnUnknownChat(const std::string& chat_id) const;

    void pollLoop();
    void answerCallbackQuery(const std::string& callback_query_id) const;  // 버튼 스피너 해제

    std::string bot_token_;
    std::string default_chat_id_;
    std::map<int, std::vector<std::string>> guardians_by_channel_;
    std::map<int, std::string> guardians_by_resident_;
    std::vector<std::string> staff_chat_ids_;
    bool guardian_alerts_ = true;

    // chat_id별 무음 만료 시각(로컬 자정). 없으면 무음 아님.
    mutable std::mutex mute_mutex_;
    std::map<std::string, std::chrono::system_clock::time_point> mute_until_;

    mutable std::mutex warn_mutex_;
    mutable std::set<std::string> warned_unknown_;

    CommandHandler on_command_;
    CallbackHandler on_callback_;
    std::thread poll_thread_;
    std::atomic<bool> polling_{false};
    int64_t update_offset_ = 0;  // getUpdates offset (처리한 update_id+1)
};
