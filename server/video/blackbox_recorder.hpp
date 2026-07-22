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

// 채널 하나의 최근 preSec_초 압축 영상(H.264 패킷)을 링버퍼로 들고 있다가,
// trigger() 호출 시 [트리거 이전 preSec_초 + 이후 postSec_초] 구간을 mp4로
// 저장한다("블랙박스"). 디코딩·재인코딩 없이 압축 패킷을 그대로 remux하므로
// CPU·메모리 부담이 거의 없다(720p 수 Mbps 스트림 기준 채널당 버퍼 수 MB 수준).
//
// 주의: 여기서 저장하는 영상은 privacy_masker를 거치기 전의 원본(마스킹 없음)
// 압축 스트림이다 — 마스킹은 디코딩 후 화면 송출용 프레임에만 적용되고,
// 블랙박스는 그 이전 단계(압축 패킷)를 그대로 담기 때문. 증거 영상 용도로는
// 의도된 동작일 수 있으나, 마스킹된 클립이 필요하면 별도 처리가 필요하다.
class BlackboxRecorder {
public:
    // 저장 완료 시 호출: (채널, 저장된 파일 경로, 이벤트 발생 시각 unix ms)
    using ClipReadyCallback =
        std::function<void(int channel, const std::string& path, int64_t eventUnixMs)>;

    BlackboxRecorder(int channel, std::string outputDir,
                     double preSec = 5.0, double postSec = 5.0);
    ~BlackboxRecorder();

    BlackboxRecorder(const BlackboxRecorder&) = delete;
    BlackboxRecorder& operator=(const BlackboxRecorder&) = delete;

    // RTSP 연결(재연결 포함) 성공 시 1회 호출 — SPS/PPS 등 코덱 파라미터와
    // 타임베이스를 등록해야 나중에 mp4로 remux할 수 있다.
    void setStreamInfo(const AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen);

    // 디코딩 전 압축 비디오 패킷마다 호출한다 (RtspAvClient 수신 스레드에서 실행).
    void onPacket(const AVPacket* pkt);

    // 이벤트(낙상 등) 발생 시 호출. 지금까지 버퍼된 구간 + 이후 postSec_초를
    // 하나의 mp4로 저장 예약한다. 짧은 시간 안에 재호출되면(재트리거) 저장
    // 예정 구간을 그만큼 연장한다. 파일명에 쓰인 이벤트 시각(unix ms)을
    // 반환하므로, 호출자가 이 값을 Qt 쪽 이벤트 알림과 맞춰 클립 파일명을
    // 재구성할 수 있게 한다.
    int64_t trigger(const std::string& eventType);

    // 이벤트가 걸린 채(armed) 서버가 종료되는 경우 등, 이후 postSec_초를
    // 못 채우더라도 지금까지 버퍼만이라도 강제로 저장하고 싶을 때 호출.
    // armed 상태가 아니면 아무 것도 하지 않는다.
    void flush();

    void onClipReady(ClipReadyCallback cb) { onClipReady_ = std::move(cb); }

private:
    struct PacketRecord {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t dts = 0;
        int flags = 0;
    };

    void flushLocked();  // mutex_ 보유 상태에서 호출 — mp4로 remux 후 버퍼 비움

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

    bool armed_ = false;  // true면 postSec_ 구간 수집 중(그동안 pre 버퍼 트리밍 정지)
    int64_t eventUnixMs_ = 0;
    std::string eventType_;
    std::chrono::steady_clock::time_point postDeadline_{};

    ClipReadyCallback onClipReady_;
};