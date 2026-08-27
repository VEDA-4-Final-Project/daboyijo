#pragma once

#include <array>
#include <chrono>
#include <map>
#include <mutex>

#include <vector>

#include <opencv2/core.hpp>

#include "detection.hpp"
#include "pose_estimator.hpp"

// [데모 오버레이] MoveNet 관절을 송출 영상 위에 그려 "AI가 지금 보고 있는 것"을
// 눈에 보이게 만드는 보관소. 시연 영상용 기능이라 기본은 꺼져 있고,
// cameras.conf 의 pose_overlay=1 일 때만 배선된다(main.cpp).
//
// 왜 서버에서 그리나 — Qt로 나가는 건 JPEG 한 장뿐이고(protocol/video_stream.h)
// 관절 좌표를 실어 보낼 통로가 없다. 프로토콜·Qt를 건드리지 않고 보여주려면
// 인코딩 전 프레임에 직접 그리는 게 가장 짧은 길이다.
//
// 스레드: put()은 채널별 AI 워커 스레드가, draw()는 채널별 파이프라인 스레드가
// 부른다. 서로 다른 스레드라 mutex_로 보호한다(둘 다 아주 짧은 임계구역).
class PoseOverlay {
public:
    using Keypoints =
        std::array<PoseEstimator::Keypoint, PoseEstimator::kNumKeypoints>;

    // 추론 1회 결과 저장.
    //   kp_in_crop  : 크롭 이미지 기준 0~1 좌표 (PoseEstimator::toCropNorm 적용 후)
    //   crop_norm   : 그 크롭이 프레임에서 차지한 영역 (프레임 기준 0~1)
    //   anchor_norm : 그 크롭을 뜰 때 쓴 WiseAI bbox (프레임 기준 0~1).
    //                 draw()의 지연 보정 기준점 — 아래 draw() 주석 참조.
    //   lying       : isLyingPose() 판정 결과 (색으로 표시)
    void put(int channel, int object_id, const Keypoints& kp_in_crop,
             const cv::Rect2f& crop_norm, const cv::Rect2f& anchor_norm,
             bool lying);

    // 이 채널의 최근 관절을 image 위에 그린다(제자리 수정). 오래된 결과는
    // 그리지 않는다 — 사람이 사라졌는데 점만 남아 있으면 더 이상하다.
    //
    // dets: 이 프레임의 감지 좌표(DetectionStore::predictedAt 결과 — 촬영시각까지
    // 속도 외삽된 최신 bbox). 관절은 최대 한 추론 주기(pose_interval_sec) 전
    // 사진의 것이라 그대로 찍으면 사람보다 뒤처져 보인다. 그래서 "추론 당시
    // bbox → 이 프레임 bbox" 변환을 관절 뭉치에 통째로 먹여 위치를 따라가게 한다.
    // (자세의 '모양'은 여전히 한 주기 전 것이지만, 눈이 쫓는 '위치'는 맞는다)
    void draw(int channel, cv::Mat& image,
              const std::vector<Detection>& dets) const;

private:
    struct Entry {
        int object_id = 0;  // 지연 보정 때 이 프레임의 bbox를 찾는 키
        Keypoints kp;       // 크롭 기준 0~1
        cv::Rect2f box;     // 프레임 기준 0~1 (크롭 영역)
        cv::Rect2f anchor;  // 프레임 기준 0~1 (추론 당시 WiseAI bbox)
        bool lying = false;
        std::chrono::steady_clock::time_point at;
    };

    mutable std::mutex mutex_;
    std::map<int, std::map<int, Entry>> channels_;  // 채널 → (객체 → 최근 관절)
};
