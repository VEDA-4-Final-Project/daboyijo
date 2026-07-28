#pragma once

#include <opencv2/core.hpp>
#include <map>
#include <mutex>
#include <vector>
#include "detection.hpp"

class PrivacyMasker {
public:
    explicit PrivacyMasker(int blur_kernel_size = 31);
    ~PrivacyMasker() = default;

    // 낙상 이벤트 발생 시 호출. 넘어진 사람(Human ObjectId)과 그 시점 무게중심
    // (정규화 0~1)을 기억해 둔다 — restoreFallen()이 그 사람 얼굴만 골라 노출한다.
    void reportFall(int channel, int object_id, float cx, float cy);

    // Qt/봇의 "확인" 신호를 받아 낙상 상태를 해제(→ 다시 전원 블러).
    void clearFall(int channel);

    // 이 채널에 확인 대기 중인 낙상이 있는지.
    bool hasFall(int channel) const;

    // 프레임에 프라이버시 마스킹(전원 블러) 적용. 낙상 여부와 무관하게 항상
    // 모든 얼굴을 블러한다 — 이 결과가 Gemini/외부로 나가는 "전원 블러본".
    void process(int channel, cv::Mat& image, const std::vector<Detection>& detections);

    // 전원 블러본(image) 위에, 낙상자 얼굴 영역만 clean(블러 전 원본)에서 되살려
    // "그 사람만 노출, 나머지는 블러" 상태로 만든다. 관제 Qt·보호자 텔레그램용.
    // 낙상자를 특정 못 하면 아무것도 되살리지 않는다(=전원 블러 유지, 안전측).
    void restoreFallen(int channel, cv::Mat& image, const cv::Mat& clean,
                       const std::vector<Detection>& detections) const;

private:
    // 낙상자 추적 정보
    struct FallTarget {
        int object_id = 0;  // 넘어진 Human의 ObjectId (얼굴 Detection의 parent_id와 매칭)
        float cx = 0;       // 무게중심 (정규화) — parent_id 매칭 실패 시 근접 폴백용
        float cy = 0;
    };

    // 얼굴 Detection 하나의 블러/복원 대상 영역(패딩 포함)과 타원 마스크 계산.
    // 유효한 영역이면 true. blur와 restore가 동일 기하를 쓰도록 공유하는 헬퍼.
    static bool computeFaceRoi(const Detection& d, const cv::Mat& image,
                               cv::Rect& roi, cv::Mat& mask);

    int blur_kernel_size_;
    mutable std::mutex mutex_;
    // 채널별 낙상자 정보 (존재 = 확인 대기 중인 낙상)
    std::map<int, FallTarget> fall_targets_;
};
