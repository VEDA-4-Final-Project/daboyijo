#include "nvr_module.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
constexpr auto kRetentionPeriod = std::chrono::seconds(60);
}  // namespace

NvrModule::NvrModule(std::string storagePath, int retentionHours,
                     double segmentMinutes, int httpPort)
    : storagePath_(std::move(storagePath)), retentionHours_(retentionHours),
      segmentMinutes_(segmentMinutes), httpPort_(httpPort),
      http_(httpPort_, storagePath_) {
    if (storagePath_.empty()) {
        std::fprintf(stderr, "[NVR] 저장 경로 미설정 — 연속 녹화 비활성화\n");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(storagePath_, ec);
    if (ec) {
        std::fprintf(stderr,
                     "[NVR] 저장 경로 생성 실패(%s): %s — 연속 녹화 비활성화\n",
                     storagePath_.c_str(), ec.message().c_str());
        return;
    }

    // 쓰기 가능 확인 — USB 미장착/읽기전용 마운트 등을 기동 시점에 걸러낸다
    const std::string probePath = storagePath_ + "/.nvr_write_probe";
    FILE* f = std::fopen(probePath.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[NVR] 저장 경로 쓰기 불가(%s) — 연속 녹화 비활성화\n",
                     storagePath_.c_str());
        return;
    }
    std::fclose(f);
    std::remove(probePath.c_str());

    enabled_ = true;
    retention_running_.store(true);
    retention_ = std::thread(&NvrModule::retentionLoop, this);
}

NvrModule::~NvrModule() {
    if (!enabled_) return;
    retention_running_.store(false);
    retention_cv_.notify_all();
    if (retention_.joinable()) retention_.join();
}

void NvrModule::retentionLoop() {
    while (retention_running_.load()) {
        {
            std::unique_lock<std::mutex> lock(retention_mutex_);
            retention_cv_.wait_for(lock, kRetentionPeriod,
                                   [this] { return !retention_running_.load(); });
        }
        if (!retention_running_.load()) break;

        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        const int64_t maxAgeMs =
            static_cast<int64_t>(retentionHours_) * 3600LL * 1000LL;

        try {
            for (const auto& entry :
                 std::filesystem::directory_iterator(storagePath_)) {
                if (!entry.is_regular_file()) continue;
                // .part(쓰는 중인 세그먼트)는 extension()이 ".part"라 자동 제외됨
                if (entry.path().extension() != ".mp4") continue;

                const std::string stem = entry.path().stem().string();  // "ch{n}_{startMs}"
                int ch = 0;
                long long startMs = 0;
                if (std::sscanf(stem.c_str(), "ch%d_%lld", &ch, &startMs) != 2) continue;

                if (nowMs - startMs > maxAgeMs) {
                    std::error_code rmEc;
                    std::filesystem::remove(entry.path(), rmEc);
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            // USB 분리 등으로 스캔 중 디렉터리가 사라진 경우 — 다음 주기에 재시도
            std::fprintf(stderr, "[NVR] 보존정책 스캔 오류: %s\n", e.what());
        }
    }
}

void NvrModule::startHttp() {
    if (!enabled_) return;
    if (!http_.start()) {
        std::fprintf(stderr,
                     "경고: NVR 클립 HTTP 서버 시작 실패 (포트 %d) — "
                     "녹화는 계속되지만 Qt에서 목록/재생은 안 됨\n",
                     httpPort_);
    }
}

void NvrModule::stopHttp() {
    if (!enabled_) return;
    http_.stop();
}

void NvrModule::attachChannel(RtspAvClient& client) {
    if (!enabled_) return;
    const int ch = client.channel();
    auto recorder = std::make_unique<NvrRecorder>(ch, storagePath_, segmentMinutes_);

    NvrRecorder* recorder_ptr = recorder.get();
    client.addStreamReadyCallback(
        [recorder_ptr](const AVCodecParameters* cp, int tbn, int tbd) {
            recorder_ptr->setStreamInfo(cp, tbn, tbd);
        });
    client.addPacketCallback([recorder_ptr](const AVPacket* pkt) {
        recorder_ptr->onPacket(pkt);
    });

    std::lock_guard<std::mutex> lock(recorders_mutex_);
    recorders_[ch] = std::move(recorder);
}

void NvrModule::flushAll() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(recorders_mutex_);
    for (auto& entry : recorders_) {
        entry.second->flush();
    }
}
