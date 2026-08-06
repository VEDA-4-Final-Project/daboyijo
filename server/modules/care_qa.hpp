#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>

#include "snapshot_buffer.hpp"
#include "telegram_module.hpp"
#include "vlm_client.hpp"

// [케어 봇] 보호자 상호작용 오케스트레이터.
//
// 텔레그램 동작은 "버튼 메뉴" 기반이다:
//   · 보호자가 무슨 메시지를 보내든 → 버튼 메뉴를 띄운다(handleMessage).
//   · 버튼 클릭(handleCallback) 처리:
//       - "now"     : 해당 채널 최근 키프레임 → VLM → 상황 설명 회신(+스냅샷)
//       - "contact" : 요양사·관리자 연락처 회신
//       - "mute"    : 이 채널 알림 오늘 무음/해제 토글(자정 자동 해제)
//       - "help"    : 사용법 안내
//
// VLM 왕복이 수 초 걸리므로 상황 조회는 짧은 detached 스레드에서 하고 폴링
// 루프(telegram_module)를 막지 않는다.
class CareQaModule {
public:
    // snapshots     : 버퍼 A(전원 블러본) — Gemini/평상시 사진용
    // snapshots_fall: 버퍼 B(낙상 선택본) — 낙상 시 보호자에게 보내는 사진용
    CareQaModule(SnapshotBuffer& snapshots, SnapshotBuffer& snapshots_fall,
                 VlmClient& vlm, TelegramModule& telegram)
        : snapshots_(snapshots), snapshots_fall_(snapshots_fall), vlm_(vlm),
          telegram_(telegram) {}

    // 📞 연락처 버튼이 회신할 연락처. main.cpp에서 config 값으로 주입.
    void setContacts(std::string caregiver, std::string manager) {
        contact_caregiver_ = std::move(caregiver);
        contact_manager_ = std::move(manager);
    }

    // TelegramModule의 커맨드 핸들러로 등록. 내용과 무관하게 메뉴를 띄운다.
    void handleMessage(int channel, const std::string& chat_id,
                       const std::string& text);

    // TelegramModule의 콜백 핸들러로 등록. 눌린 버튼(data)에 따라 동작.
    void handleCallback(int channel, const std::string& chat_id,
                        const std::string& data);

    // 낙상 확정 시 호출(main.cpp의 fall 콜백). 해당 채널 스냅샷 → VLM 상황 설명
    // → 보호자에게 자동 전송. 지속 낙상 스팸/비용 방지로 채널당 쿨다운 적용.
    void reportFall(int channel);

private:
    // 채널별 버튼 메뉴 전송(🔕/🔔 라벨은 현재 무음 상태에 맞춰 바뀐다).
    void sendMenu(int channel, const std::string& chat_id);

    SnapshotBuffer& snapshots_;       // 버퍼 A: 전원 블러본
    SnapshotBuffer& snapshots_fall_;  // 버퍼 B: 낙상 선택본
    VlmClient& vlm_;
    TelegramModule& telegram_;
    std::string contact_caregiver_;
    std::string contact_manager_;

    // 낙상 자동 리포트 쿨다운 (채널별 마지막 리포트 시각)
    std::mutex report_mutex_;
    std::map<int, std::chrono::steady_clock::time_point> last_report_;
};
