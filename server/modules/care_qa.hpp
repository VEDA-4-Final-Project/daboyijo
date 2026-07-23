#pragma once

#include <functional>

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

    CareQaModule(SnapshotBuffer& snapshots, VlmClient& vlm,
                 TelegramModule& telegram, ConfirmCallback on_confirm)
        : snapshots_(snapshots), vlm_(vlm), telegram_(telegram),
          on_confirm_(std::move(on_confirm)) {}

    // TelegramModule의 커맨드 핸들러로 등록. channel<0이면 방 매핑 불가.
    void handleMessage(int channel, const std::string& chat_id,
                       const std::string& text);

private:
    SnapshotBuffer& snapshots_;
    VlmClient& vlm_;
    TelegramModule& telegram_;
    ConfirmCallback on_confirm_;
};
