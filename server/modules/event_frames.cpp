#include "event_frames.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

// 사건 후 몇 초 기다렸다 뽑을지. 스냅샷 버퍼는 최근 5초만 보관하므로, 2.5초
// 뒤에 5초 구간을 뽑으면 [사건-2.5초, 사건+2.5초] — 넘어지기 직전부터 넘어진
// 직후까지가 대략 가운데에 온다.
constexpr double kCaptureDelaySec = 2.5;
constexpr double kCaptureSpanSec = 5.0;

}  // namespace

std::string EventFrameStore::framePath(const std::string& clip_base, int i) const {
    return dir_ + "/" + clip_base + "_f" + std::to_string(i) + ".jpg";
}

void EventFrameStore::captureAsync(int channel, int64_t evt_ms,
                                   const std::string& type) {
    // 파일명은 main.cpp가 insertEvent에 넘기는 clip 이름과 반드시 같은 규칙.
    const std::string base =
        "ch" + std::to_string(channel) + "_" + std::to_string(evt_ms) + "_" + type;

    SnapshotBuffer* snaps = &snaps_;
    const EventFrameStore* self = this;
    std::thread([snaps, self, channel, base]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(kCaptureDelaySec));
        const auto frames =
            snaps->recentKeyframes(channel, kFrames, kCaptureSpanSec);
        if (frames.empty()) {
            std::fprintf(stderr, "[event_frames] ch%d 저장할 프레임 없음 (%s)\n",
                         channel + 1, base.c_str());
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(self->dir_, ec);
        for (size_t i = 0; i < frames.size(); ++i) {
            std::ofstream f(self->framePath(base, static_cast<int>(i)),
                            std::ios::binary);
            f.write(reinterpret_cast<const char*>(frames[i].data()),
                    static_cast<std::streamsize>(frames[i].size()));
        }
        std::fprintf(stderr, "[event_frames] ch%d 사건 프레임 %zu장 저장 (%s)\n",
                     channel + 1, frames.size(), base.c_str());
    }).detach();
}

std::vector<EventFrameStore::Jpeg> EventFrameStore::load(
    const std::string& clip_url) const {
    std::vector<Jpeg> out;
    if (clip_url.empty()) return out;
    // 경로 조작 방어 — clip_url은 DB에서 오지만 파일명만 허용한다.
    if (clip_url.find('/') != std::string::npos ||
        clip_url.find('\\') != std::string::npos ||
        clip_url.find("..") != std::string::npos) {
        return out;
    }
    std::string base = clip_url;
    const auto dot = base.rfind(".mp4");
    if (dot != std::string::npos) base.erase(dot);

    for (int i = 0; i < kFrames; ++i) {
        std::ifstream f(framePath(base, i), std::ios::binary);
        if (!f) break;  // 있는 데까지만 (저장이 8장 미만이었을 수 있다)
        out.emplace_back(std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>());
    }
    return out;
}
