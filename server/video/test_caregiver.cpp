#include "caregiver_detector.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "사용법: %s <이미지경로>\n", argv[0]);
        return 1;
    }
    cv::Mat img = cv::imread(argv[1], cv::IMREAD_COLOR);
    if (img.empty()) {
        std::fprintf(stderr, "이미지 로드 실패: %s\n", argv[1]);
        return 1;
    }

    CaregiverDetector det;
    bool result = det.isCaregiver(img);
    std::printf("판정 결과: %s\n", result ? "보호사 인식됨" : "인식 안 됨");
    return 0;
}