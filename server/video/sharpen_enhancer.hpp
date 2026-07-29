#pragma once

#include <opencv2/core.hpp>
#include <vector>
#include "detection.hpp"

// [선명도 보정] WiseAI가 감지한 "사람(Human)" 바운딩 박스 영역에만
// 언샤프 마스킹을 적용해 선명도를 높이는 송출 가공 단계.
//
// ★ 블러(PrivacyMasker)와 완전히 독립적으로 동작한다:
//   - 이 모듈은 오직 type == "Human" 박스만 건드린다.
//   - 얼굴 블러는 "Head"/"Face" 박스를 건드린다.
//   - main.cpp에서 이 stage를 블러 stage보다 "앞"에 등록하므로,
//     사람 몸통을 선명하게 만든 뒤 그 위에 얼굴 블러가 덮인다.
//     → 몸통은 선명 + 얼굴은 블러(프라이버시 유지)가 동시에 성립.
class SharpenEnhancer {
public:
    // amount : 샤프닝 강도(언샤프 마스킹 가중치). 0이면 효과 없음, 클수록 강함.
    // sigma  : 언샤프용 가우시안 블러 반경. 클수록 넓은 윤곽을 강조.
    explicit SharpenEnhancer(double amount = 0.5, double sigma = 3.0);
    ~SharpenEnhancer() = default;

    // 프레임의 사람 영역에만 선명도 보정 적용. 사람이 없으면 아무 것도 안 함.
    void process(int channel, cv::Mat& image, const std::vector<Detection>& detections);

private:
    double amount_;
    double sigma_;
};
