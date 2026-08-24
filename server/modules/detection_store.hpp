#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "detection.hpp"

// [공용 인프라] 채널별 감지 결과 이력 저장소.
//
// RTSP 메타데이터 콜백(수신 스레드)이 push()로 넣고, 영상 파이프라인이
// 프레임 시각과 가장 가까운 감지를 closestTo()로 꺼내 쓴다.
// 영상이 밀리더라도 과거 좌표를 확실히 매칭하도록 최근 history_sec_초 분량을
// 시간 기준으로 보관한다 (모자이크가 영상보다 앞서나가는 버그 방지).
//
// 모든 메서드는 스레드 안전 — 기능 모듈은 이 파일을 수정할 일이 없다.
class DetectionStore {
public:
    explicit DetectionStore(double historySec = 5.0)
        : history_sec_(historySec) {}

    // 메타데이터 콜백마다 호출. captured_at은 메타 PTS로 환산한 촬영 시각
    // (영상 프레임 PTS와 같은 타임라인). 오래된 이력은 시간 기준으로 정리한다.
    void push(int channel, std::vector<Detection> dets,
              std::chrono::steady_clock::time_point captured_at);

    // 주어진 시각과 생성 시각 차이가 가장 작은 감지 결과를 반환 (없으면 빈 벡터)
    std::vector<Detection> closestTo(
        int channel, std::chrono::steady_clock::time_point t) const;

    // closestTo()에 속도 외삽을 더한 버전. 메타데이터는 ~200ms 간격(5fps)으로만
    // 오는데 영상은 최대 40fps로 그려지므로, 새 메타데이터가 오기 전까지는 같은
    // 좌표가 최대 6~8프레임 재사용돼 빠른 움직임에서 블러가 밀린다. 직전 두
    // 메타데이터 프레임에서 object_id별 이동 속도를 구해 t 시점으로 위치를
    // 예측 이동시켜, 렌더링 시점의 실제 위치에 더 가깝게 맞춘다.
    std::vector<Detection> predictedAt(
        int channel, std::chrono::steady_clock::time_point t) const;

    // 가장 최근 감지 결과 (상태 리포트용, 없으면 빈 벡터)
    std::vector<Detection> latest(int channel) const;

private:
    struct TimestampedDets {
        std::chrono::steady_clock::time_point timestamp;
        std::vector<Detection> detections;
    };

    const double history_sec_;
    mutable std::mutex mutex_;
    std::map<int, std::deque<TimestampedDets>> history_;
};
