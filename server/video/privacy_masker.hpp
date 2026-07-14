#pragma once

#include <opencv2/core.hpp>
#include <map>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include "detection.hpp"

class PrivacyMasker {
public:
    // unmask_duration_sec: 낙상 발생 시 블러를 해제할 시간 (기본 10초)
    // blur_kernel_size: 블러 세기 (기본 31, 반드시 홀수)
    PrivacyMasker(double unmask_duration_sec = 10.0, int blur_kernel_size = 31);
    ~PrivacyMasker() = default;

    // 낙상 이벤트 발생 시 호출 (해당 채널의 블러 해제 타이머 가동)[cite: 1]
    void reportFall(int channel);

    // 프레임에 동적 마스킹(블러) 적용[cite: 1]
    void process(int channel, cv::Mat& image, const std::vector<Detection>& detections);

private:
    double unmask_duration_sec_;
    int blur_kernel_size_;

    std::mutex mutex_;
    // 채널별 마지막 낙상 이벤트 발생 시점을 저장[cite: 1]
    std::map<int, std::chrono::steady_clock::time_point> fall_events_; 
};