#include "blackbox_module.hpp"

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

}  // namespace

BlackboxModule::BlackboxModule() : http_(kClipHttpPort, kBlackboxDir) {
    std::filesystem::create_directories(kBlackboxDir);
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
        std::printf("[ch%d] 블랙박스 저장 완료: %s (이벤트 %lld)\n", rch,
                    path.c_str(), static_cast<long long>(eventMs));
    });

    BlackboxRecorder* recorder_ptr = recorder.get();
    client.setStreamReadyCallback(
        [recorder_ptr](const AVCodecParameters* cp, int tbn, int tbd) {
            recorder_ptr->setStreamInfo(cp, tbn, tbd);
        });
    client.setPacketCallback([recorder_ptr](const AVPacket* pkt) {
        recorder_ptr->onPacket(pkt);
    });
    recorders_[ch] = std::move(recorder);
}

int64_t BlackboxModule::trigger(int channel) {
    auto it = recorders_.find(channel);
    if (it == recorders_.end()) return 0;
    return it->second->trigger();
}

void BlackboxModule::flushAll() {
    for (auto& entry : recorders_) {
        entry.second->flush();
    }
}
