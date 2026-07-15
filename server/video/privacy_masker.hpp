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

    // 낙상 이벤트 발생 시 호출 (해당 채널의 블러 해제)
    void reportFall(int channel);

    // Qt의 확인 신호를 받아 마스킹을 원상복구(ON) 시킬 함수
    void clearFall(int channel);

    // 프레임에 동적 마스킹(블러) 적용
    void process(int channel, cv::Mat& image, const std::vector<Detection>& detections);

private:
    int blur_kernel_size_;
    std::mutex mutex_;
    //채널별 낙상 블러 해제 플래그 (true면 블러 해제 유지)
    std::map<int, bool> fall_unmasked_;
};