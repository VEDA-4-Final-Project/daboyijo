#include "blackbox_module.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

// ── 블랙박스 튜닝값 ──────────────────────────────────────────────
// 디코딩 전 압축 패킷을 그대로 버퍼링/remux하므로 채널당 버퍼는 수 MB
// 수준(720p 수 Mbps 스트림 기준)이라 라즈베리파이에서도 부담이 거의 없다.
const std::string kBlackboxDir = "blackbox_clips";
constexpr double kBlackboxPreSec = 5.0;
constexpr double kBlackboxPostSec = 5.0;
// 저장된 클립을 Qt가 QMediaPlayer로 바로 재생할 수 있게 서빙하는 HTTP 포트.
constexpr int kClipHttpPort = 5501;
// 마감 시각이 지난 클립을 확인하는 주기. 사람이 체감할 만큼만 빠르면 되고,
// 평상시엔 recorders_를 한 번 훑고 끝이라 부담이 없다.
constexpr auto kWatchdogPeriod = std::chrono::milliseconds(1000);

}  // namespace

BlackboxModule::BlackboxModule() : http_(kClipHttpPort, kBlackboxDir) {
    std::filesystem::create_directories(kBlackboxDir);
    watchdog_running_.store(true);
    watchdog_ = std::thread(&BlackboxModule::watchdogLoop, this);
}

BlackboxModule::~BlackboxModule() {
    watchdog_running_.store(false);
    watchdog_cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
}

void BlackboxModule::watchdogLoop() {
    while (watchdog_running_.load()) {
        {
            std::unique_lock<std::mutex> lock(watchdog_mutex_);
            watchdog_cv_.wait_for(lock, kWatchdogPeriod,
                                  [this] { return !watchdog_running_.load(); });
        }
        if (!watchdog_running_.load()) break;

        std::lock_guard<std::mutex> lock(recorders_mutex_);
        for (auto& entry : recorders_) {
            entry.second->flushIfDue();
        }
    }
}

void BlackboxModule::startHttp() {
    if (!http_.start()) {
        std::fprintf(stderr,
                     "경고: 블랙박스 클립 HTTP 서버 시작 실패 (포트 %d) — "
                     "블랙박스 저장은 계속되지만 Qt에서 재생은 안 됨\n",
                     kClipHttpPort);
    }
}

void BlackboxModule::stopHttp() {
    http_.stop();
}

void BlackboxModule::attachChannel(RtspAvClient& client) {
    const int ch = client.channel();
    auto recorder = std::make_unique<BlackboxRecorder>(
        ch, kBlackboxDir, kBlackboxPreSec, kBlackboxPostSec);
    recorder->onClipReady([](int rch, const std::string& path, int64_t eventMs) {
        // std::printf("[ch%d] 블랙박스 저장 완료: %s (이벤트 %lld)\n", rch,
        //             path.c_str(), static_cast<long long>(eventMs));
    });

    BlackboxRecorder* recorder_ptr = recorder.get();
    client.setStreamReadyCallback(
        [recorder_ptr](const AVCodecParameters* cp, int tbn, int tbd) {
            recorder_ptr->setStreamInfo(cp, tbn, tbd);
        });
    client.setPacketCallback([recorder_ptr](const AVPacket* pkt) {
        recorder_ptr->onPacket(pkt);
    });

    std::lock_guard<std::mutex> lock(recorders_mutex_);
    recorders_[ch] = std::move(recorder);
}

int64_t BlackboxModule::trigger(int channel, const std::string& eventType) {
    std::lock_guard<std::mutex> lock(recorders_mutex_);
    auto it = recorders_.find(channel);
    if (it == recorders_.end()) return 0;
    return it->second->trigger(eventType);
}

void BlackboxModule::flushAll() {
    std::lock_guard<std::mutex> lock(recorders_mutex_);
    for (auto& entry : recorders_) {
        entry.second->flush();
    }
}
