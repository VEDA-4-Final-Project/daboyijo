#pragma once

#include <opencv2/core.hpp>
#include <vector>
#include "detection.hpp"

// [밝기 보정] 송출/스냅샷 프레임 전체에 감마 보정을 적용해 저조도(야간
// 요양원 방) 가시성을 높이는 송출 가공 단계.
//
// ★ 샤프닝과 달리 사람 박스가 아니라 프레임 전체를 다룬다. 생성자에서 미리
//   계산한 256칸 LUT 한 장으로 픽셀당 테이블 룩업만 하므로 비용이 매우 싸다
//   (GaussianBlur 같은 무거운 연산이 없어 부하 예산 부담이 거의 없다).
//
// ★ AI(낙상/요양사)는 stage 실행 전의 "깨끗한 원본"을 받으므로(video_pipeline
//   .cpp 참조), 이 stage는 검출 입력을 바꾸지 않는다 — 순수 송출 화질 개선.
//
//   gamma > 1 : 어두운 영역을 끌어올려 밝게 (야간 권장)
//   gamma = 1 : 무효과
//   gamma < 1 : 더 어둡게
class BrightnessEnhancer {
public:
    // gamma : 감마 보정 계수. 1이면 효과 없음, 클수록 어두운 부분이 밝아진다.
    explicit BrightnessEnhancer(double gamma = 1.5);
    ~BrightnessEnhancer() = default;

    // 프레임 전체에 감마 보정 적용 (detections는 사용하지 않음).
    void process(int channel, cv::Mat& image,
                 const std::vector<Detection>& detections);

private:
    cv::Mat lut_;    // 1x256 8UC1 룩업 테이블 (생성자에서 1회 계산)
    bool enabled_;   // gamma≈1이면 stage 자체를 건너뜀
};
