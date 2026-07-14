#include "pose_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <opencv2/imgproc.hpp>

#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

namespace {

// 어깨-엉덩이 벡터가 수직에서 이만큼(도) 이상 기울면 "누움"으로 본다.
// 서 있으면 거의 0도(수직), 완전히 누우면 90도에 가깝다. 잠정값 — 실측 캡처로 튜닝.
constexpr float kLyingAngleDeg = 55.0f;
// 어깨/엉덩이 관절 신뢰도가 이 미만이면 그 관절은 못 믿는 것으로 보고 판정 보류.
constexpr float kMinJointScore = 0.25f;
constexpr float kPi = 3.14159265358979323846f;

}  // namespace

struct PoseEstimator::Impl {
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter> interpreter;
    TfLiteDelegate* xnnpack = nullptr;
    int input_w = 192, input_h = 192;
    bool input_is_uint8 = true;

    ~Impl() {
        if (xnnpack) TfLiteXNNPackDelegateDelete(xnnpack);
    }
};

PoseEstimator::PoseEstimator(const std::string& modelPath, int numThreads)
    : impl_(std::make_unique<Impl>()) {
    impl_->model = tflite::FlatBufferModel::BuildFromFile(modelPath.c_str());
    if (!impl_->model) {
        std::fprintf(stderr, "[pose] 모델 로드 실패: %s\n", modelPath.c_str());
        return;
    }

    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder builder(*impl_->model, resolver);
    builder(&impl_->interpreter);
    if (!impl_->interpreter) {
        std::fprintf(stderr, "[pose] 인터프리터 생성 실패\n");
        return;
    }
    impl_->interpreter->SetNumThreads(numThreads);

    // XNNPACK 델리게이트 — 파이썬 벤치(tflite_runtime, 평균 36ms)와 동일한 가속 경로.
    // 적용 실패해도 치명적이지 않음(느린 CPU 경로로 폴백) — 그냥 경고만.
    TfLiteXNNPackDelegateOptions xopts = TfLiteXNNPackDelegateOptionsDefault();
    xopts.num_threads = numThreads;
    impl_->xnnpack = TfLiteXNNPackDelegateCreate(&xopts);
    if (impl_->interpreter->ModifyGraphWithDelegate(impl_->xnnpack) != kTfLiteOk) {
        std::fprintf(stderr, "[pose] XNNPACK 델리게이트 적용 실패 — CPU 기본 경로로 진행\n");
    }

    if (impl_->interpreter->AllocateTensors() != kTfLiteOk) {
        std::fprintf(stderr, "[pose] AllocateTensors 실패\n");
        impl_->interpreter.reset();
        return;
    }

    const TfLiteTensor* input = impl_->interpreter->input_tensor(0);
    impl_->input_h = input->dims->data[1];
    impl_->input_w = input->dims->data[2];
    impl_->input_is_uint8 = (input->type == kTfLiteUInt8);

    std::fprintf(stderr, "[pose] MoveNet 로드 완료 (%dx%d, %s)\n", impl_->input_w,
                 impl_->input_h, impl_->input_is_uint8 ? "uint8" : "float32");
}

PoseEstimator::~PoseEstimator() = default;

bool PoseEstimator::isReady() const {
    return impl_ && impl_->interpreter != nullptr;
}

bool PoseEstimator::estimate(const cv::Mat& personCropBgr,
                             std::array<Keypoint, kNumKeypoints>& out) const {
    if (!isReady() || personCropBgr.empty()) return false;

    // BGR → RGB, 비율 유지 리사이즈 + 레터박스(검정 여백)로 모델 입력 크기에 맞춘다.
    // (MoveNet 표준 전처리: tf.image.resize_with_pad와 동일한 방식)
    // 단순 stretch resize는 누운 사람처럼 bbox가 심하게 납작(예: 469x101)해지면
    // 정사각형으로 늘리는 과정에서 몸이 왜곡돼 관절 신뢰도가 무너진다
    // (실측: stretch 시 신뢰도 0.1대로 붕괴 → 레터박스로 해결).
    cv::Mat rgb;
    cv::cvtColor(personCropBgr, rgb, cv::COLOR_BGR2RGB);

    const float scale = std::min(static_cast<float>(impl_->input_w) / rgb.cols,
                                 static_cast<float>(impl_->input_h) / rgb.rows);
    const int new_w = std::max(1, static_cast<int>(std::round(rgb.cols * scale)));
    const int new_h = std::max(1, static_cast<int>(std::round(rgb.rows * scale)));
    cv::Mat scaled;
    cv::resize(rgb, scaled, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

    cv::Mat resized(impl_->input_h, impl_->input_w, rgb.type(), cv::Scalar(0, 0, 0));
    const int off_x = (impl_->input_w - new_w) / 2;
    const int off_y = (impl_->input_h - new_h) / 2;
    scaled.copyTo(resized(cv::Rect(off_x, off_y, new_w, new_h)));

    if (!resized.isContinuous()) resized = resized.clone();

    TfLiteTensor* input = impl_->interpreter->input_tensor(0);
    const size_t n_bytes =
        static_cast<size_t>(resized.total()) * resized.elemSize();
    if (impl_->input_is_uint8) {
        std::memcpy(input->data.uint8, resized.data, n_bytes);
    } else {
        // float32 입력(비양자화) 모델용 — 0~1 정규화
        float* dst = input->data.f;
        const uint8_t* src = resized.data;
        for (size_t i = 0; i < n_bytes; ++i) dst[i] = src[i] / 255.0f;
    }

    if (impl_->interpreter->Invoke() != kTfLiteOk) {
        std::fprintf(stderr, "[pose] 추론 실패\n");
        return false;
    }

    const TfLiteTensor* output = impl_->interpreter->output_tensor(0);
    // 출력: [1,1,17,3] = (y,x,score) × 17, 정규화 0~1. 입력이 int8이어도
    // 출력은 float32로 나오는 모델이 많아 방어적으로 둘 다 처리한다.
    if (output->type == kTfLiteFloat32) {
        const float* d = output->data.f;
        for (int i = 0; i < kNumKeypoints; ++i) {
            out[i].y = d[i * 3 + 0];
            out[i].x = d[i * 3 + 1];
            out[i].score = d[i * 3 + 2];
        }
    } else if (output->type == kTfLiteUInt8 || output->type == kTfLiteInt8) {
        const auto* d = output->data.uint8;
        const float scale = output->params.scale;
        const int zero = output->params.zero_point;
        for (int i = 0; i < kNumKeypoints; ++i) {
            out[i].y = (d[i * 3 + 0] - zero) * scale;
            out[i].x = (d[i * 3 + 1] - zero) * scale;
            out[i].score = (d[i * 3 + 2] - zero) * scale;
        }
    } else {
        std::fprintf(stderr, "[pose] 알 수 없는 출력 타입: %d\n",
                     static_cast<int>(output->type));
        return false;
    }
    return true;
}

bool PoseEstimator::isLyingPose(const std::array<Keypoint, kNumKeypoints>& kp) const {
    const auto& ls = kp[kLeftShoulder];
    const auto& rs = kp[kRightShoulder];
    const auto& lh = kp[kLeftHip];
    const auto& rh = kp[kRightHip];
    if (ls.score < kMinJointScore || rs.score < kMinJointScore ||
        lh.score < kMinJointScore || rh.score < kMinJointScore) {
        std::fprintf(stderr,
                     "[pose] 신뢰도 부족 판정보류 — 어깨(%.2f,%.2f) 엉덩이(%.2f,%.2f) 기준=%.2f\n",
                     ls.score, rs.score, lh.score, rh.score, kMinJointScore);
        return false;  // 신뢰도 부족 — 판정 보류(낙상 아님으로 보수적 처리)
    }

    const float shoulder_x = (ls.x + rs.x) / 2.0f;
    const float shoulder_y = (ls.y + rs.y) / 2.0f;
    const float hip_x = (lh.x + rh.x) / 2.0f;
    const float hip_y = (lh.y + rh.y) / 2.0f;

    const float dx = hip_x - shoulder_x;
    const float dy = hip_y - shoulder_y;
    if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) return false;

    // 수직(dy) 기준 기울기 각도. 0도=수직(서 있음), 90도=수평(누움). 방향 무관.
    const float angle_from_vertical =
        std::atan2(std::fabs(dx), std::fabs(dy)) * 180.0f / kPi;

    const bool lying = angle_from_vertical >= kLyingAngleDeg;
    std::fprintf(stderr, "[pose] 기울기=%.1f도 (기준 %.1f) → %s\n",
                 angle_from_vertical, kLyingAngleDeg, lying ? "누움" : "서있음");
    return lying;
}

bool PoseEstimator::isLyingDown(const cv::Mat& personCropBgr) const {
    std::array<Keypoint, kNumKeypoints> kp;
    if (!estimate(personCropBgr, kp)) return false;
    return isLyingPose(kp);
}
