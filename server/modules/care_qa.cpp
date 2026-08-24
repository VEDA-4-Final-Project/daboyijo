#include "care_qa.hpp"

#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

constexpr int kKeyframes = 3;       // VLM에 보낼 키프레임 수
constexpr double kSpanSec = 5.0;    // 최근 몇 초 구간에서 뽑을지
constexpr double kReportCooldownSec = 60.0;  // 낙상 자동 리포트 최소 간격

// 📋 전체 방 현황은 방 수만큼 Gemini 를 부른다. 무료 티어 일일 한도(RPD)를 방 수
// 배로 태우므로 방당 1장만 보낸다 — 📷 단일 조회(3장)보다 정확도는 떨어지지만
// "지금 이 방 사람이 어떤 자세인가" 수준은 1장으로도 판단된다.
constexpr int kOverviewKeyframes = 1;

const char* kHelpGuardian =
    "🤖 케어봇 사용법 (보호자)\n"
    "· 아무 메시지나 보내면 버튼 메뉴가 나와요.\n"
    "· 📷 지금 상황 보기 : 방 상황을 사진과 함께 알려드려요\n"
    "· 🔍 영상 검색 : \"어제 저녁에 낙상 있었어?\"처럼 물어보면 지난 기록을 찾아드려요\n"
    "· 📞 연락처 : 요양사·관리자 연락처\n"
    "· 🔕/🔔 알림 : 오늘 침상 이탈 알림을 껐다 켰다 (자정 자동 해제)\n"
    "  ※ 낙상·생체이상 알림은 안전상 무음과 무관하게 항상 전송돼요.";

const char* kHelpStaff =
    "🤖 케어봇 사용법 (요양사)\n"
    "· 아무 메시지나 보내면 방 목록이 나와요. 방을 고르면 동작 메뉴로 들어갑니다.\n"
    "· 📷 지금 상황 보기 : 그 방 상황을 사진과 함께 요약합니다\n"
    "· 🔍 영상 검색 : 그 방의 지난 기록을 자연어로 찾습니다\n"
    "· 🛏 침상 현황 : 담당 방 침대별로 재실/이탈을 바로 보여줍니다(즉답)\n"
    "· 📋 전체 방 현황 : 담당 방을 전부 요약합니다(방 수만큼 시간이 걸립니다)\n"
    "· 📞 연락처 : 관리자 연락처\n"
    "※ 요양사 계정에는 무음 기능이 없습니다 — 근무 중 알림은 항상 전송됩니다.";

// 📷 지금 상황 보기 버튼이 VLM에 던지는 고정 프롬프트.
// (사용자 원문을 그대로 넘기던 방식과 달리, 용도를 요양 상황 설명으로 못박아
//  엉뚱한 질문·프롬프트 인젝션을 원천 차단한다.)
const char* kNowQuestionGuardian =
    "요양원 실내 CCTV 사진입니다(사생활 보호로 얼굴은 블러 처리됨). 화면 속 "
    "어르신이 지금 무엇을 하고 계신지(앉아 계심/누워 계심/걷는 중/식사 중 등)와 "
    "특이사항이 있는지 보호자에게 알리듯 두세 문장으로 차분하고 다정하게 알려주세요. "
    "의학적 진단은 하지 말고 보이는 상황만 설명하세요.";

// ★ 요양사용은 톤이 다르다. 보호자는 안심이 목적이고, 요양사는 "지금 저 방에
//   가봐야 하나"를 판단하는 게 목적이라 사실을 짧게 나열하는 편이 쓸모 있다.
const char* kNowQuestionStaff =
    "요양원 실내 CCTV 사진입니다(사생활 보호로 얼굴은 블러 처리됨). 요양사가 순회 "
    "여부를 판단하는 데 쓰도록, 화면 속 어르신의 현재 자세와 위치, 움직임 여부, "
    "눈에 띄는 이상(바닥에 누움/침대 밖/오래 같은 자세 등)을 두세 문장으로 간결하게 "
    "사실 위주로 적어 주세요. 의학적 진단은 하지 말고 보이는 것만 적으세요.";

// 📋 전체 방 현황용. 방마다 한 줄로 붙일 거라 한 문장만 받는다.
const char* kOverviewQuestion =
    "요양원 실내 CCTV 사진입니다(사생활 보호로 얼굴은 블러 처리됨). 화면 속 "
    "어르신의 현재 자세와 위치를 한 문장으로만 간결하게 적어 주세요. 사람이 보이지 "
    "않으면 \"사람 없음\"이라고만 적으세요. 의학적 진단은 하지 마세요.";

// "now:2" → {"now", 2}. 접미사가 없으면 채널은 -1(= 핸들러가 받은 값을 쓴다).
// ★ 채널을 버튼에 실어 보내는 이유는 care_qa.hpp 상단 주석 참고(무상태 유지).
std::pair<std::string, int> parseCallback(const std::string& data) {
    const size_t colon = data.find(':');
    if (colon == std::string::npos) return {data, -1};
    const std::string verb = data.substr(0, colon);
    const std::string arg = data.substr(colon + 1);
    if (arg.empty() || !std::all_of(arg.begin(), arg.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return {verb, -1};
    }
    return {verb, std::stoi(arg)};
}

nlohmann::json button(const std::string& text, const std::string& data) {
    nlohmann::json b;
    b["text"] = text;
    b["callback_data"] = data;
    return b;
}

}  // namespace

std::string CareQaModule::roomPrefix(int channel, Role role) {
    if (role != Role::Staff) return std::string();
    return TelegramModule::chLabel(channel) + " ";
}

std::vector<int> CareQaModule::channelsFor(const std::string& chat_id,
                                           Role role) const {
    // 요양사는 전 채널 담당이므로 카메라 목록이 곧 담당 목록이다.
    if (role == Role::Staff) return channels_;
    return telegram_.channelsForGuardian(chat_id);
}

// 기본은 내용과 무관하게 메뉴를 띄운다 — 단, 🔍 영상 검색 버튼을 누른 직후의
// chat_id는 예외로, 이번 메시지를 그 방의 검색 질의로 소비한다(1회성).
void CareQaModule::handleMessage(int channel, const std::string& chat_id,
                                 Role role, const std::string& text) {
    int search_channel = -1;
    bool awaitingSearch = false;
    {
        std::lock_guard<std::mutex> lock(search_mutex_);
        auto it = awaiting_search_.find(chat_id);
        if (it != awaiting_search_.end()) {
            search_channel = it->second;
            awaiting_search_.erase(it);
            awaitingSearch = true;
        }
    }
    if (awaitingSearch) {
        if (search_channel < 0) {
            telegram_.sendMessage(chat_id,
                "검색할 방을 특정하지 못했어요. 메뉴에서 방을 먼저 골라 주세요.");
            sendMenu(channel, chat_id, role);
            return;
        }
        // Gemini+DB 왕복은 폴링 루프를 막지 않도록 detached 스레드에서(now와 동일 패턴).
        std::thread([this, search_channel, chat_id, role, text]() {
            const std::string answer = video_search_.search(search_channel, text);
            telegram_.sendMessage(chat_id, answer);
            sendMenu(search_channel, chat_id, role);
        }).detach();
        return;
    }

    (void)text;  // 검색 대기 중이 아니면 내용은 쓰지 않는다 — 뭘 쓰든 메뉴로 안내
    sendMenu(channel, chat_id, role);
}

void CareQaModule::sendMenu(int channel, const std::string& chat_id, Role role) {
    if (channel >= 0) {
        sendActionMenu(channel, chat_id, role);
        return;
    }
    const auto chs = channelsFor(chat_id, role);
    if (chs.empty()) {
        telegram_.sendMessage(
            chat_id,
            role == Role::Staff
                ? "담당할 방이 설정돼 있지 않아요. 관리자에게 카메라 채널 설정을 "
                  "확인해 달라고 요청해 주세요."
                : "계정에 방(채널)이 연결돼 있지 않아요. 관리자에게 "
                  "telegram_chat_id_<채널번호> 설정을 요청해 주세요.");
        return;
    }
    if (chs.size() == 1) {
        sendActionMenu(chs.front(), chat_id, role);  // 방이 하나면 고를 필요가 없다
        return;
    }
    sendRoomPicker(chat_id, role, chs);
}

void CareQaModule::sendRoomPicker(const std::string& chat_id, Role role,
                                  const std::vector<int>& channels) {
    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json row = nlohmann::json::array();
    for (size_t i = 0; i < channels.size(); ++i) {
        const int ch = channels[i];
        row.push_back(button("🏠 " + std::to_string(ch + 1) + "번 방",
                             "menu:" + std::to_string(ch)));
        // 한 줄에 두 개씩 — 폰 화면에서 세 개 이상은 라벨이 잘린다.
        if (row.size() == 2 || i + 1 == channels.size()) {
            rows.push_back(row);
            row = nlohmann::json::array();
        }
    }
    // 담당 방 전체를 대상으로 하는 동작은 방을 고르기 전 화면에 두는 게 자연스럽다.
    if (role == Role::Staff) {
        nlohmann::json row_all = nlohmann::json::array();
        row_all.push_back(button("🛏 침상 현황", "beds"));
        row_all.push_back(button("📋 전체 방 현황", "overview"));
        rows.push_back(row_all);
    }

    nlohmann::json kb;
    kb["inline_keyboard"] = rows;
    telegram_.sendMessage(chat_id, "어느 방을 보시겠어요?", kb.dump());
}

void CareQaModule::sendActionMenu(int channel, const std::string& chat_id,
                                  Role role) {
    const std::string ch_arg = ":" + std::to_string(channel);

    nlohmann::json row_now = nlohmann::json::array();
    row_now.push_back(button("📷 지금 상황 보기", "now" + ch_arg));
    row_now.push_back(button("🔍 영상 검색", "search" + ch_arg));

    nlohmann::json row_info = nlohmann::json::array();
    row_info.push_back(button("📞 연락처", "contact"));
    row_info.push_back(button("ℹ️ 도움말", "help"));

    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(row_now);
    rows.push_back(row_info);

    // ★ 무음은 보호자 전용이다. 요양사에게 근무 중 알림을 끄는 버튼을 주지 않는다.
    //   보호자 알림 자체를 끈 운영(telegram_guardian_alerts=0)에서는 끌 알림이
    //   없으므로 보호자에게도 버튼을 띄우지 않는다 — 눌러도 아무 일이 안 일어나는
    //   버튼을 두면 "껐는데 왜 오지"보다 나쁜 "껐다고 믿는" 오해를 만든다.
    if (role == Role::Guardian && telegram_.guardianAlertsEnabled()) {
        const bool muted = telegram_.isMuted(chat_id);
        nlohmann::json row_mute = nlohmann::json::array();
        row_mute.push_back(button(
            muted ? "🔔 알림 다시 켜기" : "🔕 오늘 알림 끄기", "mute"));
        rows.push_back(row_mute);
    }

    // 요양사 전용 — 담당 방 전체를 대상으로 하는 동작.
    if (role == Role::Staff) {
        nlohmann::json row_staff = nlohmann::json::array();
        row_staff.push_back(button("🛏 침상 현황", "beds"));
        row_staff.push_back(button("📋 전체 방 현황", "overview"));
        rows.push_back(row_staff);
    }

    // 볼 수 있는 방이 여럿일 때만 "다른 방" 버튼을 붙인다.
    if (channelsFor(chat_id, role).size() > 1) {
        nlohmann::json row_rooms = nlohmann::json::array();
        row_rooms.push_back(button("🏠 다른 방 보기", "rooms"));
        rows.push_back(row_rooms);
    }

    nlohmann::json kb;
    kb["inline_keyboard"] = rows;
    telegram_.sendMessage(
        chat_id,
        roomPrefix(channel, role) + "무엇을 도와드릴까요?", kb.dump());
}

void CareQaModule::sendBedStatus(const std::string& chat_id, Role role) {
    const auto chs = channelsFor(chat_id, role);
    if (chs.empty()) {
        telegram_.sendMessage(chat_id, "담당할 방이 설정돼 있지 않아요.");
        return;
    }
    // DB 왕복은 짧지만, 폴링 루프에서 직접 돌리면 커넥션이 막힐 때 봇 전체가 멎는다.
    std::thread([this, chat_id, role, chs]() {
        const auto zones = db_.loadRoiZones();
        std::string msg = "🛏 침상 현황";
        bool any_mapped = false;

        for (const int ch : chs) {
            // 누워 있는 사람 = 아직 안 닫힌 재실 세션. 나머지가 이탈 중이다.
            const auto in_bed = db_.getResidentsInBed(ch);
            msg += "\n\n" + TelegramModule::chLabel(ch);
            bool found = false;
            for (const auto& z : zones) {
                if (z.camera_id != ch || z.resident_id <= 0) continue;
                found = any_mapped = true;
                const std::string name = db_.getResidentName(z.resident_id);
                const bool lying = std::find(in_bed.begin(), in_bed.end(),
                                             z.resident_id) != in_bed.end();
                msg += "\n· 침대" + std::to_string(z.roi_id + 1) + " " +
                       (name.empty() ? std::string("이름 미등록") : name) +
                       (lying ? " — 재실" : " — 이탈 중");
            }
            if (!found) msg += "\n· 등록된 침대가 없어요";
        }
        if (!any_mapped) {
            msg += "\n\n※ 침대 ROI에 입소자가 매핑돼 있어야 이름과 재실 여부가 "
                   "표시됩니다. Qt 관제 화면에서 침대를 그리고 입소자를 연결해 주세요.";
        }
        telegram_.sendMessage(chat_id, msg);
        sendMenu(-1, chat_id, role);
    }).detach();
}

void CareQaModule::sendOverview(const std::string& chat_id, Role role) {
    const auto chs = channelsFor(chat_id, role);
    if (chs.empty()) {
        telegram_.sendMessage(chat_id, "담당할 방이 설정돼 있지 않아요.");
        return;
    }
    if (!vlm_.available()) {
        telegram_.sendMessage(chat_id,
            "지금은 상황 확인 기능이 설정돼 있지 않아요. (관리자 설정 필요)");
        sendMenu(-1, chat_id, role);
        return;
    }
    // 방 수만큼 VLM 왕복이라 수십 초가 걸릴 수 있다. 먼저 받았다는 신호를 주지
    // 않으면 사용자가 버튼을 계속 눌러 그만큼 Gemini 호출이 배로 늘어난다.
    telegram_.sendMessage(chat_id,
        "📋 " + std::to_string(chs.size()) +
        "개 방을 확인하고 있어요. 잠시만 기다려 주세요…");

    std::thread([this, chat_id, role, chs]() {
        std::string msg = "📋 전체 방 현황";
        for (const int ch : chs) {
            msg += "\n\n" + TelegramModule::chLabel(ch) + " ";
            auto frames =
                snapshots_.recentKeyframes(ch, kOverviewKeyframes, kSpanSec);
            if (frames.empty()) {
                msg += "영상이 들어오지 않음";
                continue;
            }
            const std::string a = vlm_.describe(frames, kOverviewQuestion);
            msg += a.empty() ? std::string("확인 실패 — 잠시 후 다시 시도") : a;
        }
        telegram_.sendMessage(chat_id, msg);
        sendMenu(-1, chat_id, role);
    }).detach();
}

void CareQaModule::handleCallback(int channel, const std::string& chat_id,
                                  Role role, const std::string& data) {
    const auto [verb, arg_channel] = parseCallback(data);
    const int ch = arg_channel >= 0 ? arg_channel : channel;

    // ── 🏠 방 선택 화면으로 ──
    if (verb == "rooms") {
        const auto chs = channelsFor(chat_id, role);
        if (chs.size() > 1) {
            sendRoomPicker(chat_id, role, chs);
        } else {
            sendMenu(-1, chat_id, role);
        }
        return;
    }

    // ── 🛏 침상 현황 / 📋 전체 방 현황 ──
    // 담당 방 전체가 대상이라 채널 인자가 없다. 권한 범위는 channelsFor 가 잡으므로
    // 보호자가 눌러도 자기 방 밖은 보이지 않는다.
    if (verb == "beds") {
        sendBedStatus(chat_id, role);
        return;
    }
    if (verb == "overview") {
        sendOverview(chat_id, role);
        return;
    }

    // ── 📞 연락처 ──
    // 요양사에게는 관리자만 준다 — 요양사 개인 연락처를 요양사끼리 돌릴 이유가 없고,
    // 보호자에게 나가는 것과 같은 문면을 쓰면 누구 번호인지 헷갈린다.
    if (verb == "contact") {
        const std::string manager =
            contact_manager_.empty() ? "미등록" : contact_manager_;
        if (role == Role::Staff) {
            telegram_.sendMessage(chat_id, "📞 연락처\n· 관리자: " + manager);
        } else {
            const std::string caregiver =
                contact_caregiver_.empty() ? "미등록" : contact_caregiver_;
            telegram_.sendMessage(
                chat_id,
                "📞 연락처\n· 요양사: " + caregiver + "\n· 관리자: " + manager);
        }
        sendMenu(ch, chat_id, role);  // 대답 후 메뉴 다시 표시
        return;
    }

    // ── ℹ️ 도움말 ──
    if (verb == "help") {
        telegram_.sendMessage(chat_id,
                              role == Role::Staff ? kHelpStaff : kHelpGuardian);
        sendMenu(ch, chat_id, role);
        return;
    }

    // ── 🔕/🔔 알림 토글 (보호자 전용) ──
    if (verb == "mute") {
        if (role == Role::Staff) {
            // 버튼을 준 적이 없으니 정상 경로로는 오지 않는다. 오래된 메시지의
            // 버튼을 다시 누른 경우를 대비해 조용히 막고 이유를 알린다.
            telegram_.sendMessage(chat_id,
                "요양사 계정은 알림을 끌 수 없어요 — 근무 중 알림은 항상 "
                "전송됩니다.");
            sendMenu(ch, chat_id, role);
            return;
        }
        const bool now_muted = telegram_.toggleMute(chat_id);
        telegram_.sendMessage(chat_id,
            now_muted ? "🔕 오늘 침상 이탈 알림을 껐어요. 자정에 자동으로 다시 "
                        "켜집니다.\n(낙상·생체이상 알림은 안전상 항상 전송됩니다.)"
                      : "🔔 침상 이탈 알림을 다시 켰어요.");
        sendMenu(ch, chat_id, role);  // 토글 후 메뉴 재표시(🔕/🔔 라벨도 갱신됨)
        return;
    }

    // 아래부터는 방이 특정돼야 하는 동작이다.
    if (ch < 0) {
        sendMenu(-1, chat_id, role);  // 방부터 고르게 한다
        return;
    }
    // ★ 버튼에 채널이 실려 오므로 누를 때마다 권한을 다시 본다. 오래된 메시지의
    //   버튼이 계속 살아 있어서, 담당이 바뀐 뒤에도 예전 방이 열릴 수 있다.
    const auto allowed = channelsFor(chat_id, role);
    if (std::find(allowed.begin(), allowed.end(), ch) == allowed.end()) {
        telegram_.sendMessage(chat_id, "그 방을 볼 권한이 없어요.");
        sendMenu(-1, chat_id, role);
        return;
    }

    // ── 📷 지금 상황 보기 ──
    if (verb == "now") {
        if (!vlm_.available()) {
            telegram_.sendMessage(chat_id,
                "지금은 상황 확인 기능이 설정돼 있지 않아요. (관리자 설정 필요)");
            sendMenu(ch, chat_id, role);
            return;
        }
        // 수 초 걸리는 VLM 왕복은 폴링 루프를 막지 않도록 별도 스레드에서.
        // VLM 답변 전송이 끝난 뒤 메뉴를 다시 띄우려면 this가 필요하다(sendMenu).
        std::thread([this, ch, chat_id, role]() {
            auto frames = snapshots_.recentKeyframes(ch, kKeyframes, kSpanSec);
            if (frames.empty()) {
                telegram_.sendMessage(chat_id,
                    "지금 영상이 들어오지 않아 상황을 확인하기 어려워요.");
                sendMenu(ch, chat_id, role);
                return;
            }
            const char* q =
                role == Role::Staff ? kNowQuestionStaff : kNowQuestionGuardian;
            std::string answer = vlm_.describe(frames, q);
            if (answer.empty()) {
                telegram_.sendMessage(chat_id,
                    "일시적으로 상황을 확인하기 어려워요. 잠시 후 다시 시도해 주세요.");
                sendMenu(ch, chat_id, role);
                return;
            }
            telegram_.sendMessage(chat_id, roomPrefix(ch, role) + answer);
            telegram_.sendPhoto(chat_id, frames.back(), "지금 상황 📷");
            sendMenu(ch, chat_id, role);  // 상황 설명·사진 전송 후 메뉴 다시 표시
        }).detach();
        return;
    }

    // ── 🔍 영상 검색 ──
    if (verb == "search") {
        {
            std::lock_guard<std::mutex> lock(search_mutex_);
            awaiting_search_[chat_id] = ch;  // 다음 메시지를 이 방 검색어로 소비
        }
        telegram_.sendMessage(chat_id,
            "🔎 " + roomPrefix(ch, role) +
            "찾으시는 상황을 말씀해 주세요.\n"
            "예: \"어제 저녁에 낙상 있었어?\", \"오늘 오전에 침대에서 나간 적 있어?\"");
        // 메뉴는 다시 안 띄운다 — 지금은 다음 메시지를 검색어로 기다리는 중.
        return;
    }

    // 알 수 없는 버튼 — 메뉴 다시 표시
    sendMenu(ch, chat_id, role);
}

void CareQaModule::reportFall(int channel, const Subject& who) {
    // 수신자를 먼저 확정한다 — 입소자가 특정되면 그분 보호자에게만, 미상이면 그 방
    // 보호자 전원에게. 요양사는 항상 포함된다.
    const auto recipients = telegram_.recipientsFor(channel, who);
    if (recipients.empty()) return;

    // 쿨다운: 지속 낙상으로 콜백이 반복돼도 kReportCooldownSec 이내엔 1회만.
    // ★ 키가 (채널, 입소자)라 같은 방의 다른 분 낙상은 따로 카운트된다.
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto key = std::make_pair(channel, who.resident_id);
        auto it = last_report_.find(key);
        if (it != last_report_.end() &&
            std::chrono::duration<double>(now - it->second).count() <
                kReportCooldownSec) {
            return;
        }
        last_report_[key] = now;
    }

    // VLM 왕복(수 초)은 fall 콜백(AI 워커 스레드)을 막지 않도록 detached 스레드에서.
    SnapshotBuffer* snapA = &snapshots_;       // 전원 블러본 (Gemini용)
    SnapshotBuffer* snapB = &snapshots_fall_;  // 낙상 선택본 (수신자 사진용)
    VlmClient* vlm = &vlm_;
    TelegramModule* tg = &telegram_;
    std::thread([snapA, snapB, vlm, tg, channel, who, recipients]() {
        const std::string room = TelegramModule::chLabel(channel);
        const std::string sub = TelegramModule::subjectLabel(who);

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
        if (!shot.empty()) {
            for (const auto& r : recipients) {
                tg->sendPhoto(r.chat_id, shot, room + " 낙상 감지 시점 📷");
            }
        }

        // ── 2) Gemini 상황 설명 ── 반드시 전원 블러본(버퍼 A)만 외부로.
        // 사진은 이미 나갔으므로, 이 왕복이 느려도 사진 타이밍엔 영향 없다.
        // ★ 설명은 수신자가 몇 명이든 한 번만 만든다 — 사람 수만큼 Gemini를 부르면
        //   같은 사진을 중복으로 태워 무료 티어 일일 한도가 그만큼 빨리 마른다.
        if (!vlm->available()) return;
        auto blurred = snapA->recentKeyframes(channel, kKeyframes, kSpanSec);
        if (blurred.empty()) return;

        const std::string q =
            "요양원 실내 CCTV에서 낙상 감지 시스템이 방금 낙상을 감지한 직후의 "
            "사진들입니다(촬영 순서, 사생활 보호로 얼굴은 블러 처리됨). 화면 속 "
            "어르신이 바닥이나 매트 위에 누워·엎드려·주저앉아 계시면, 이는 침상에서의 "
            "휴식이 아니라 넘어져 쓰러진 '낙상' 상황입니다. 어르신의 현재 자세(옆으로 "
            "누움/엎드림/뒤로 넘어짐 등)와 움직임 여부, 손·다리 위치를 두세 문장으로 "
            "차분하게 전해 주세요. 의학적 진단이나 부상 여부는 단정하지 말고, 보이는 "
            "상황만 설명하세요.";
        const std::string answer = vlm->describe(blurred, q);
        if (answer.empty()) return;

        // 같은 설명을 역할에 맞는 머리말과 함께 나눠 보낸다.
        for (const auto& r : recipients) {
            const std::string head =
                r.role == TelegramModule::Role::Staff
                    ? "🚨 낙상 " + room + " " + sub + " — 현재 상황\n"
                    : "🚨 낙상 감지 — 현재 상황\n";
            tg->sendMessage(r.chat_id, head + answer);
        }
    }).detach();
}
