#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
struct AVCodecParameters;
struct AVPacket;
}

// 채널 하나의 블랙박스 — 최근 preSec_초 압축 패킷을 링버퍼에 들고 있다가
// trigger() 시 [이전 preSec_초 + 이후 postSec_초] 를 mp4 로 저장
// 디코딩·재인코딩 없이 remux 만 해서 부담이 거의 없음 (채널당 버퍼 수 MB)
//
// 주의: privacy_masker 이전의 원본 스트림이라 얼굴 마스킹이 안 걸림
// 마스킹은 디코딩 후 송출 프레임에만 적용되고 여기는 그 전 단계
class BlackboxRecorder {
public:
    // 저장 완료 콜백 — (채널, 파일 경로, 이벤트 시각 unix ms)
    using ClipReadyCallback =
        std::function<void(int channel, const std::string& path, int64_t eventUnixMs)>;

    BlackboxRecorder(int channel, std::string outputDir,
                     double preSec = 5.0, double postSec = 5.0);
    ~BlackboxRecorder();

    BlackboxRecorder(const BlackboxRecorder&) = delete;
    BlackboxRecorder& operator=(const BlackboxRecorder&) = delete;

    // RTSP 연결·재연결마다 1회 — SPS/PPS 와 타임베이스를 등록해야 remux 가능
    void setStreamInfo(const AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen);

    // 디코딩 전 압축 패킷마다 호출 (RtspAvClient 수신 스레드)
    void onPacket(const AVPacket* pkt);

    // 이벤트 발생 시 호출 — 저장 예약 후 이벤트 시각(unix ms) 반환
    // 반환값으로 Qt 쪽 알림과 클립 파일명을 맞출 수 있음
    //
    // 같은 종류로 재호출: 구간만 연장하고 최초 시각 유지 (한 사건 = 한 클립)
    // 다른 종류: 지금 것을 끊어 저장하고 새 클립 시작
    //   파일명이 (채널, 시각, 종류)라 한 파일에 두 종류를 못 담음
    int64_t trigger(const std::string& eventType);

    // postSec_ 를 못 채워도 버퍼만이라도 강제 저장 (종료 시 유실 방지)
    // armed 가 아니면 아무 동작 없음
    void flush();

    // 마감 시각이 지났으면 저장 — BlackboxModule 감시 스레드가 주기 호출
    // onPacket 은 패킷이 와야 불리므로 트리거 직후 RTSP 가 끊기면 아무도 저장을 안 함
    void flushIfDue();

    void onClipReady(ClipReadyCallback cb) { onClipReady_ = std::move(cb); }

private:
    struct PacketRecord {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t dts = 0;
        int flags = 0;
    };

    void flushLocked();      // mutex_ 보유 상태 전용 — mp4 remux 후 버퍼 비움
    bool dueLocked() const;  // mutex_ 보유 상태 전용 — 저장할 때가 됐는지

    const int channel_;
    const std::string outputDir_;
    const double preSec_;
    const double postSec_;

    std::mutex mutex_;
    std::deque<PacketRecord> buf_;

    AVCodecParameters* codecpar_ = nullptr;
    int tbNum_ = 1;
    int tbDen_ = 90000;
    bool haveStreamInfo_ = false;

    bool armed_ = false;  // postSec_ 구간 수집 중 — 그동안 pre 버퍼 트리밍 정지
    int64_t eventUnixMs_ = 0;
    std::string eventType_;
    std::chrono::steady_clock::time_point postDeadline_{};
    // 한 클립의 절대 상한 — 재트리거로 안 밀리므로 버퍼가 끝없이 자라는 걸 막음
    std::chrono::steady_clock::time_point armDeadline_{};

    ClipReadyCallback onClipReady_;
};