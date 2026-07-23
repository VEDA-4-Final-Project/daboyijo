#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

// [공용 인프라] 채널별 최근 송출 JPEG 프레임 링버퍼.
//
// 영상 파이프라인이 프레임을 인코딩·송출할 때마다 put()으로 최신 JPEG를
// 넣고(=블러 마스킹이 이미 적용된 프레임), 케어 봇이 보호자 질문을 받으면
// recentKeyframes()로 최근 구간에서 고르게 퍼진 몇 장을 꺼내 VLM에 보낸다.
//
// 시간 기준(history_sec_)으로만 보관해 오래된 프레임은 자동 폐기한다.
// 모든 메서드는 스레드 안전 — 파이프라인(쓰기)과 봇 스레드(읽기)가 다르다.
class SnapshotBuffer {
public:
    using Jpeg = std::vector<unsigned char>;

    explicit SnapshotBuffer(double historySec = 5.0) : history_sec_(historySec) {}

    // 파이프라인이 매 송출 프레임마다 호출. jpeg는 복사되어 보관된다.
    void put(int channel, const Jpeg& jpeg,
             std::chrono::steady_clock::time_point t);

    // 최근 spanSec 구간에서 시간상 고르게 퍼진 최대 n장을 오래된→최신 순으로 반환.
    // 보관된 게 n장보다 적으면 있는 만큼만. 없으면 빈 벡터.
    std::vector<Jpeg> recentKeyframes(int channel, int n = 3,
                                      double spanSec = 5.0) const;

private:
    struct Stamped {
        std::chrono::steady_clock::time_point t;
        Jpeg jpeg;
    };

    const double history_sec_;
    mutable std::mutex mutex_;
    std::map<int, std::deque<Stamped>> frames_;
};
