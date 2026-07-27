#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "detection_store.hpp"
#include "rtsp_av_client.hpp"
#include "stream_server.hpp"
#include "system_stats.hpp"

// [공용 인프라] 5초 주기 상태 리포트 — 채널별 fps·사람 수·CPU·온도·인코딩 시간.
// VideoPipeline(메인 스레드)에서만 호출되므로 락 불필요.
class StatsReporter {
public:
    StatsReporter(const std::vector<std::unique_ptr<RtspAvClient>>& clients,
                  DetectionStore& store, StreamServer& server);

    // 프레임 1장 송출 완료마다 호출.
    //   procMs   = 프레임 전체 처리시간(resize+clone+블러+imencode+broadcast)
    //   encodeMs = 순수 imencode 소요 시간 (전체 중 인코딩이 차지하는 비중 진단용)
    void onFrameSent(int channel, size_t jpegBytes, double procMs,
                     double encodeMs);

    // 매 루프마다 호출 — 5초 지났으면 리포트 출력
    void maybeReport();

private:
    struct ChannelStats {
        uint64_t processed = 0;
        uint64_t bytes = 0;
    };

    const std::vector<std::unique_ptr<RtspAvClient>>& clients_;
    DetectionStore& store_;
    StreamServer& server_;
    SystemStats system_stats_;

    std::map<int, ChannelStats> stats_;
    double proc_ms_total_ = 0;    // 전체 처리시간 누적
    double encode_ms_total_ = 0;  // 순수 imencode 시간 누적
    uint64_t encode_count_ = 0;
    std::chrono::steady_clock::time_point last_report_;
    std::vector<uint64_t> last_counts_;
};
