#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

extern "C" {
struct AVCodecParameters;
struct AVPacket;
struct AVFormatContext;
}

// 채널 하나의 연속(NVR) 녹화 — 압축 패킷이 오는 대로 즉시 mp4 세그먼트에
// remux 한다 (디코딩·재인코딩 없음, BlackboxRecorder와 같은 원리). 다만
// BlackboxRecorder는 이벤트가 언제 터질지 몰라 pre 구간을 링버퍼로 들고
// 있다가 트리거 시점에 한꺼번에 쓰는 반면, 이건 지금 오는 패킷을 바로
// 기록하면 되므로 앱단 버퍼가 없다 — 세그먼트 하나짜리 muxer를 계속 열어두고
// 매 패킷을 그때그때 씀. segmentSec 경과 후 다음 키프레임에서 세그먼트를
// 마감하고 새로 연다(키프레임 경계여야 각 mp4가 처음부터 단독 재생 가능).
//
// 주의: privacy_masker 이전의 원본 스트림이라 얼굴 마스킹이 안 걸림(블랙박스와 동일 제약)
class NvrRecorder {
public:
    NvrRecorder(int channel, std::string outputDir, double segmentMinutes);
    ~NvrRecorder();

    NvrRecorder(const NvrRecorder&) = delete;
    NvrRecorder& operator=(const NvrRecorder&) = delete;

    // RTSP 연결·재연결마다 1회 — SPS/PPS와 타임베이스를 등록해야 remux 가능
    // 진행 중이던 세그먼트가 있으면 마감 후 새 스트림 정보로 갱신한다
    void setStreamInfo(const AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen);

    // 디코딩 전 압축 패킷마다 호출 (RtspAvClient 수신 스레드)
    void onPacket(const AVPacket* pkt);

    // 진행 중인 세그먼트를 지금 상태로 마감 (종료 시 유실 방지)
    void flush();

private:
    void closeSegmentLocked();                        // mutex_ 보유 상태 전용
    bool openSegmentLocked(const AVPacket* firstPkt);  // mutex_ 보유 상태 전용 — 키프레임으로 시작

    const int channel_;
    const std::string outputDir_;
    const double segmentSec_;

    std::mutex mutex_;

    AVCodecParameters* codecpar_ = nullptr;
    int tbNum_ = 1;
    int tbDen_ = 90000;
    bool haveStreamInfo_ = false;

    AVFormatContext* ofmt_ = nullptr;
    std::string curPath_;
    std::string curTmpPath_;
    std::chrono::steady_clock::time_point segmentStartSteady_{};

    // pts/dts 재기준(0부터 단조증가) 상태 — BlackboxRecorder::flushLocked와 같은
    // 원리를 스트리밍(패킷 단위)으로 적용
    int64_t base_ = 0;
    int64_t lastDts_ = -1;
    int64_t estDur_ = 0;
};
