#include "stats_reporter.hpp"

#include <cstdio>
#include <sstream>

StatsReporter::StatsReporter(
    const std::vector<std::unique_ptr<RtspAvClient>>& clients,
    DetectionStore& store, StreamServer& server)
    : clients_(clients),
      store_(store),
      server_(server),
      last_report_(std::chrono::steady_clock::now()),
      last_counts_(clients.size(), 0),
      last_meta_(clients.size(), 0) {}

void StatsReporter::onFrameSent(int channel, size_t jpegBytes, double procMs,
                                double prepMs, double encodeMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ch = stats_[channel];
    ch.processed += 1;
    ch.bytes += jpegBytes;
    proc_ms_total_ += procMs;
    prep_ms_total_ += prepMs;
    encode_ms_total_ += encodeMs;
    encode_count_ += 1;
}

void StatsReporter::maybeReport() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_report_);
    if (elapsed.count() < 5) return;

    if (last_counts_.size() != clients_.size()) {
        last_counts_.resize(clients_.size(), 0);
        last_meta_.resize(clients_.size(), 0);
    }

    std::ostringstream status;
    for (size_t i = 0; i < clients_.size(); ++i) {
        const int id = clients_[i]->channel();
        const uint64_t count = clients_[i]->frameCount();
        const uint64_t meta  = clients_[i]->metadataCount();   // ★ 실측용
        const double in_fps =
            static_cast<double>(count - last_counts_[i]) / elapsed.count();
        const double out_fps =
            static_cast<double>(stats_[id].processed) / elapsed.count();

        // 이 채널의 최신 감지에서 사람 수 집계
        int humans = 0;
        for (const auto& d : store_.latest(id)) {
            if (d.isHuman()) ++humans;
        }

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[ch%d] %s in %.1f out %.1ffps 메타%llu 객체%zu 사람%d  ",
                      id + 1, clients_[i]->connected() ? "OK" : "끊김", in_fps,
                      out_fps, (unsigned long long)(meta - last_meta_[i]),
                      store_.latest(id).size(), humans);
        status << buf;
        last_counts_[i] = count;
        last_meta_[i] = meta;
        stats_[id] = ChannelStats{};
    }

    const double avg_proc =
        encode_count_ ? proc_ms_total_ / encode_count_ : 0;
    const double avg_prep =
        encode_count_ ? prep_ms_total_ / encode_count_ : 0;
    const double avg_encode =
        encode_count_ ? encode_ms_total_ / encode_count_ : 0;
    const double avg_etc = avg_proc - avg_prep - avg_encode;  // 스테이지+송출
    char sys_buf[144];
    std::snprintf(
        sys_buf, sizeof(sys_buf),
        "| CPU %.0f%% %.1f°C 처리 %.1fms(준비 %.1f 인코딩 %.1f 기타 %.1f) 클라 %zu",
        system_stats_.cpuPercent(), SystemStats::socTemperature(), avg_proc,
        avg_prep, avg_encode, avg_etc, server_.clientCount());
    status << sys_buf;

    proc_ms_total_ = 0;
    prep_ms_total_ = 0;
    encode_ms_total_ = 0;
    encode_count_ = 0;
    // 로그 정리로 비활성화 — 부하 측정(fps·CPU·온도·인코딩 시간)이 필요할 때 해제.
    // 위 집계와 카운터 리셋은 그대로 돌려둔다: 그것까지 멈추면 다시 켰을 때 첫
    // 리포트가 그동안 쌓인 값이라 엉뚱하게 나온다.
    // std::printf("%s\n", status.str().c_str());
    (void)status;  // 로그 끈 동안 미사용 경고 방지 (위 주석 해제 시 같이 지울 것)
    last_report_ = now;
}
