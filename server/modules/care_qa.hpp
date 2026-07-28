#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>

#include "snapshot_buffer.hpp"
#include "telegram_module.hpp"
#include "vlm_client.hpp"

// [케어 봇] 보호자 질문 처리 오케스트레이터.
//
// 텔레그램에서 들어온 메시지를 받아 →
//   · "/확인"  : 낙상 경보 확인 콜백 호출 (블러 원상복구)
//   · "/도움"  : 사용법 안내
//   · 그 외    : 해당 채널 최근 키프레임 2~3장 → VLM → 상황 설명을 회신(+스냅샷)
//
// VLM 왕복이 수 초 걸리므로 질의 처리는 짧은 detached 스레드에서 하고 폴링
// 루프(telegram_module)를 막지 않는다.
class CareQaModule {
public:
    using ConfirmCallback = std::function<void(int channel)>;

    // snapshots     : 버퍼 A(전원 블러본) — Gemini/평상시 사진용
    // snapshots_fall: 버퍼 B(낙상 선택본) — 낙상 시 보호자에게 보내는 사진용
    CareQaModule(SnapshotBuffer& snapshots, SnapshotBuffer& snapshots_fall,
                 VlmClient& vlm, TelegramModule& telegram,
                 ConfirmCallback on_confirm)
        : snapshots_(snapshots), snapshots_fall_(snapshots_fall), vlm_(vlm),
          telegram_(telegram), on_confirm_(std::move(on_confirm)) {}

    // TelegramModule의 커맨드 핸들러로 등록. channel<0이면 방 매핑 불가.
    void handleMessage(int channel, const std::string& chat_id,
                       const std::string& text);

    // 낙상 확정 시 호출(main.cpp의 fall 콜백). 해당 채널 스냅샷 → VLM 상황 설명
    // → 보호자에게 자동 전송. 지속 낙상 스팸/비용 방지로 채널당 쿨다운 적용.
    void reportFall(int channel);

private:
    SnapshotBuffer& snapshots_;       // 버퍼 A: 전원 블러본
    SnapshotBuffer& snapshots_fall_;  // 버퍼 B: 낙상 선택본
    VlmClient& vlm_;
    TelegramModule& telegram_;
    ConfirmCallback on_confirm_;

    // 낙상 자동 리포트 쿨다운 (채널별 마지막 리포트 시각)
    std::mutex report_mutex_;
    std::map<int, std::chrono::steady_clock::time_point> last_report_;
};
