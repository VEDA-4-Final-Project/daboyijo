#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "snapshot_buffer.hpp"
#include "telegram_module.hpp"
#include "video_search_module.hpp"
#include "vlm_client.hpp"

class Database;

// [케어 봇] 보호자·요양사 상호작용 오케스트레이터.
//
// 텔레그램 동작은 "버튼 메뉴" 기반이다:
//   · 무슨 메시지를 보내든 → 버튼 메뉴를 띄운다(handleMessage).
//   · 버튼 클릭(handleCallback) 처리:
//       - "now:<ch>"     : 그 채널 최근 키프레임 → VLM → 상황 설명 회신(+스냅샷)
//       - "search:<ch>"  : 다음 메시지를 그 채널 영상 검색 질의로 소비
//       - "menu:<ch>"    : 그 방의 동작 메뉴로 진입
//       - "rooms"        : 방 선택 화면으로 돌아가기
//       - "contact"      : 연락처 회신 (역할에 따라 내용이 다르다)
//       - "mute"         : 내 알림 오늘 무음/해제 토글 (보호자 전용)
//       - "help"         : 사용법 안내
//
// ── 역할에 따라 화면이 갈린다 ──────────────────────────────────
//   Guardian(보호자) : 자기 어르신이 있는 방만. 방이 하나면 바로 동작 메뉴.
//                      무음(🔕) 버튼이 있다.
//   Staff(요양사)    : 전 채널. 먼저 방을 고르고 동작 메뉴로 들어간다.
//                      무음 버튼이 없다 — 근무 중 알림을 끄는 경로를 두지 않는다.
//
// ★ 버튼에 채널을 실어 보내는(callback_data "now:2") 이유: "지금 보고 있는 방"을
//   서버 메모리에 들고 있으면 2-Pi 구성에서 두 대가 서로 다른 상태를 갖게 된다.
//   무상태로 두면 어느 Pi가 그 클릭을 받아도 같은 방이 열린다.
//
// VLM 왕복이 수 초 걸리므로 상황 조회는 짧은 detached 스레드에서 하고 폴링
// 루프(telegram_module)를 막지 않는다.
class CareQaModule {
public:
    using Role = TelegramModule::Role;
    using Subject = TelegramModule::Subject;

    // snapshots     : 버퍼 A(전원 블러본) — Gemini/평상시 사진용
    // snapshots_fall: 버퍼 B(낙상 선택본) — 낙상 시 보내는 사진용
    // video_search  : 🔍 영상 검색 버튼이 위임하는 자연어 질의 처리기
    // db: 🛏 침상 현황이 roi_zones(침대↔입소자)와 bed_sessions(재실 여부)를 읽는다.
    CareQaModule(SnapshotBuffer& snapshots, SnapshotBuffer& snapshots_fall,
                 VlmClient& vlm, TelegramModule& telegram,
                 VideoSearchModule& video_search, Database& db)
        : snapshots_(snapshots), snapshots_fall_(snapshots_fall), vlm_(vlm),
          telegram_(telegram), video_search_(video_search), db_(db) {}

    // 📞 연락처 버튼이 회신할 연락처. main.cpp에서 config 값으로 주입.
    void setContacts(std::string caregiver, std::string manager) {
        contact_caregiver_ = std::move(caregiver);
        contact_manager_ = std::move(manager);
    }

    // 요양사 방 선택 버튼에 쓸 채널 목록. main.cpp에서 config.cameras로 주입.
    // 비어 있으면 요양사에게 방 선택지가 없어 안내 문구만 나간다.
    void setChannels(std::vector<int> channels) {
        channels_ = std::move(channels);
    }

    // TelegramModule의 커맨드 핸들러로 등록. 내용과 무관하게 메뉴를 띄운다.
    void handleMessage(int channel, const std::string& chat_id, Role role,
                       const std::string& text);

    // TelegramModule의 콜백 핸들러로 등록. 눌린 버튼(data)에 따라 동작.
    void handleCallback(int channel, const std::string& chat_id, Role role,
                        const std::string& data);

    // 낙상 확정 시 호출(main.cpp의 fall 콜백). 해당 채널 스냅샷 → VLM 상황 설명
    // → 수신자(보호자 + 요양사)에게 자동 전송.
    // ★ VLM은 한 번만 부르고 결과를 여러 명에게 나눠 보낸다 — 수신자 수만큼 부르면
    //   같은 사진을 중복으로 태워 무료 티어 일일 한도가 그만큼 빨리 마른다.
    void reportFall(int channel, const Subject& who);

private:
    // 역할·채널 상태에 맞는 화면을 고른다(방 선택 or 동작 메뉴).
    void sendMenu(int channel, const std::string& chat_id, Role role);
    void sendRoomPicker(const std::string& chat_id, Role role,
                        const std::vector<int>& channels);
    void sendActionMenu(int channel, const std::string& chat_id, Role role);

    // 🛏 침상 현황 — 담당 방 전부의 침대별 재실/이탈. DB만 보므로 즉답이고
    // Gemini 를 쓰지 않는다.
    void sendBedStatus(const std::string& chat_id, Role role);

    // 📋 전체 방 현황 — 담당 방 전부를 VLM 으로 요약.
    // ★ 방 수만큼 Gemini 를 부른다. 무료 티어 일일 한도(RPD)를 방 수 배로 태우므로
    //   방당 키프레임을 1장만 쓴다(📷 단일 조회는 3장). 느린 것도 감수 대상이라
    //   먼저 "확인 중" 안내를 보내고 결과를 나중에 한 통으로 보낸다.
    void sendOverview(const std::string& chat_id, Role role);

    // 회신 앞에 붙일 방 라벨("[ch1] "). 요양사는 방을 오가므로 어느 방 얘긴지
    // 붙여주고, 보호자는 보는 방이 하나뿐이라 매번 붙으면 잡음이라 빈 문자열.
    static std::string roomPrefix(int channel, Role role);

    // 이 사용자가 고를 수 있는 방들. 요양사는 전 채널, 보호자는 등록된 방만.
    std::vector<int> channelsFor(const std::string& chat_id, Role role) const;

    SnapshotBuffer& snapshots_;       // 버퍼 A: 전원 블러본
    SnapshotBuffer& snapshots_fall_;  // 버퍼 B: 낙상 선택본
    VlmClient& vlm_;
    TelegramModule& telegram_;
    VideoSearchModule& video_search_;
    Database& db_;
    std::string contact_caregiver_;
    std::string contact_manager_;
    std::vector<int> channels_;  // 요양사 방 선택용 전체 채널 목록

    // 낙상 자동 리포트 쿨다운.
    // ★ 키가 (채널, 입소자)인 이유: 한 방에 두 분이 계실 때 채널로만 걸면 먼저
    //   넘어진 분의 쿨다운이 다른 분의 낙상 리포트를 통째로 삼킨다.
    std::mutex report_mutex_;
    std::map<std::pair<int, int>, std::chrono::steady_clock::time_point>
        last_report_;

    // 🔍 영상 검색 버튼을 누른 chat_id → 검색할 채널. 다음 메시지(handleMessage)를
    // 메뉴가 아니라 그 채널의 검색 질의로 처리하기 위한 1회성 플래그(소비하면 제거).
    std::mutex search_mutex_;
    std::map<std::string, int> awaiting_search_;
};
