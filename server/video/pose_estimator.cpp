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
// 하체(엉덩이→무릎/발목) 벡터가 수직에서 이 각도 미만이면 "다리는 서 있음".
// 허리만 숙이면 몸통은 기울어도(신호1 발화) 하체는 수직에 가깝다 — 반면 실제로
// 누우면 몸통·하체가 함께 수평이 된다. 이 값 미만이면 신호1을 거부(허리 숙임으로
// 판단)해 세면·물건줍기 등 오탐을 막는다. 잠정값 — 실측 로그로 튜닝.
constexpr float kUprightLegDeg = 40.0f;
// 어깨/엉덩이 관절 신뢰도가 이 미만이면 그 관절은 못 믿는 것으로 보고 판정 보류.
constexpr float kMinJointScore = 0.25f;
constexpr float kPi = 3.14159265358979323846f;

// ── 정면(카메라 축 방향) 낙상 보강 신호 임계값 ──────────────────────
// 카메라 쪽으로 눕는 낙상은 몸통이 원근 단축돼 화면에서 기울지 않는다 —
// 각도 신호만으로는 못 잡는 사각지대(실측 확인). 아래 값은 실측 전 잠정값으로,
// 표준 인체 비례(어깨너비 ≈ 신장의 1/4, 어깨~발목 ≈ 신장의 3/4)에서 유도했다.
// 판정 로그에 측정값이 전부 남으므로 현장 캡처 후 그 수치로 튜닝할 것.
//
// 어깨가 엉덩이보다 화면에서 이만큼(정규화) 아래면 "상하 반전"으로 본다.
// 서 있으면 어깨는 항상 엉덩이 위 — 반전은 머리부터 고꾸라진 자세의 강한 신호.
constexpr float kInvertMarginNorm = 0.02f;
// 어깨 너비가 이보다 좁으면(측면 자세 등) 비율 판정 불가 — 그 신호만 보류.
constexpr float kMinShoulderWidthNorm = 0.03f;
// 세로 스프레드(어깨~가장 아래 관절) ÷ 어깨 너비가 기준 미만이면 "원근 단축".
// 서 있으면 발목까지 ~3.5, 무릎까지 ~2.5, 엉덩이까지 ~1.7 수준 — 카메라 쪽으로
// 누우면 세로 스프레드만 뭉개져 비율이 크게 떨어진다.
constexpr float kSpreadRatioToAnkle = 2.0f;
constexpr float kSpreadRatioToKnee = 1.6f;
constexpr float kSpreadRatioTorsoOnly = 1.0f;
// 원근 단축 신호의 주저앉기 오탐 방지 게이트 — 제자리에 쪼그려 앉으면 다리가
// 접혀 세로 스프레드는 누운 것처럼 뭉개지지만(실측: 주저앉기가 낙상으로 오탐),
// 몸통은 세워져 있어 몸통 길이 ÷ 어깨 너비가 서 있을 때(~1.2~1.7) 그대로다.
// 카메라 쪽으로 실제로 누우면 몸통 자체가 단축돼 이 비율도 함께 떨어진다.
// → 몸통 비율이 이 값 미만일 때만 원근 단축 신호를 인정한다.
constexpr float kMaxTorsoRatioForeshort = 1.0f;

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
        // 로그 정리로 비활성화 — 디버깅 시 해제
        // std::fprintf(stderr,
        //              "[pose] 신뢰도 부족 판정보류 — 어깨(%.2f,%.2f) 엉덩이(%.2f,%.2f) 기준=%.2f\n",
        //              ls.score, rs.score, lh.score, rh.score, kMinJointScore);
        return false;  // 신뢰도 부족 — 판정 보류(낙상 아님으로 보수적 처리)
    }

    const float shoulder_x = (ls.x + rs.x) / 2.0f;
    const float shoulder_y = (ls.y + rs.y) / 2.0f;
    const float hip_x = (lh.x + rh.x) / 2.0f;
    const float hip_y = (lh.y + rh.y) / 2.0f;

    const float dx = hip_x - shoulder_x;
    const float dy = hip_y - shoulder_y;
    if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) return false;

    // ── 하체 관절 수집 (신호1 하체 veto + 신호3 스프레드가 공유) ──
    // 발목을 우선하되 없으면 무릎, 둘 다 없으면 하체 정보 없음. 양쪽 관절이
    // 모두 신뢰될 때만 중점을 쓴다(한쪽만 보이면 좌표가 편향돼 각도가 흔들림).
    bool has_knee = false, has_ankle = false;
    float knee_x = 0, knee_y = 0, ankle_x = 0, ankle_y = 0;
    const auto& lk = kp[kLeftKnee];
    const auto& rk = kp[kRightKnee];
    if (lk.score >= kMinJointScore && rk.score >= kMinJointScore) {
        knee_x = (lk.x + rk.x) / 2.0f;
        knee_y = (lk.y + rk.y) / 2.0f;
        has_knee = true;
    }
    const auto& la = kp[kLeftAnkle];
    const auto& ra = kp[kRightAnkle];
    if (la.score >= kMinJointScore && ra.score >= kMinJointScore) {
        ankle_x = (la.x + ra.x) / 2.0f;
        ankle_y = (la.y + ra.y) / 2.0f;
        has_ankle = true;
    }

    // ── 하체 각도 veto ──
    // 엉덩이 → 가장 아래 신뢰관절(발목 우선, 없으면 무릎) 벡터의 수직 기울기.
    // 이 각도가 kUprightLegDeg 미만이면 "다리는 서 있음" = 허리만 숙인 자세로
    // 보고 신호1을 거부한다. 하체를 못 보면(둘 다 없음) veto 없이 기존대로.
    bool legs_upright = false;
    if (has_ankle || has_knee) {
        const float lower_x = has_ankle ? ankle_x : knee_x;
        const float lower_y = has_ankle ? ankle_y : knee_y;
        const float leg_dx = lower_x - hip_x;
        const float leg_dy = lower_y - hip_y;
        if (std::fabs(leg_dx) > 1e-6f || std::fabs(leg_dy) > 1e-6f) {
            const float leg_angle =
                std::atan2(std::fabs(leg_dx), std::fabs(leg_dy)) * 180.0f / kPi;
            legs_upright = leg_angle < kUprightLegDeg;
        }
    }

    // ── 신호 1: 몸통 기울기 ──
    // 수직(dy) 기준 기울기 각도. 0도=수직(서 있음), 90도=수평(누움). 방향 무관.
    // 옆으로 눕는 낙상을 잡는다. 단, fabs(dy)라 상하 반전은 여기 안 보인다 —
    // 그건 신호 2가 담당. 하체가 서 있으면(허리 숙임) 거부한다.
    const float angle_from_vertical =
        std::atan2(std::fabs(dx), std::fabs(dy)) * 180.0f / kPi;
    const bool by_angle = angle_from_vertical >= kLyingAngleDeg && !legs_upright;

    // ── 신호 2: 상하 반전 ──
    // y는 화면 아래로 증가. 서 있으면 항상 dy > 0 (엉덩이가 어깨 아래).
    // 카메라 쪽으로 머리부터 넘어지면 어깨가 엉덩이 밑으로 내려가 dy < 0.
    const bool inverted = dy < -kInvertMarginNorm;

    // ── 신호 3: 원근 단축 (카메라 축 방향 낙상) ──
    // 몸이 카메라를 향해 누우면 세로 방향 길이는 투영에서 뭉개지지만,
    // 어깨 너비는 넘어진 축과 수직이라 거의 그대로다. 세로 스프레드(어깨~
    // 가장 아래 신뢰 관절)를 어깨 너비로 나눈 비율이 서 있을 때보다 뚝
    // 떨어지면 누운 것으로 본다. 발목→무릎→엉덩이 순으로 신뢰되는 관절까지
    // 재고, 어디까지 쟀는지에 따라 기준을 달리 적용한다.
    const float shoulder_w = std::hypot(ls.x - rs.x, ls.y - rs.y);
    float min_y = std::min(shoulder_y, hip_y);
    float max_y = std::max(shoulder_y, hip_y);
    float ratio_limit = kSpreadRatioTorsoOnly;
    const char* extent = "엉덩이";
    if (has_knee) {
        min_y = std::min(min_y, knee_y);
        max_y = std::max(max_y, knee_y);
        ratio_limit = kSpreadRatioToKnee;
        extent = "무릎";
    }
    if (has_ankle) {
        min_y = std::min(min_y, ankle_y);
        max_y = std::max(max_y, ankle_y);
        ratio_limit = kSpreadRatioToAnkle;
        extent = "발목";
    }
    float spread_ratio = -1.0f;  // -1 = 어깨 너비가 좁아 측정 불가(측면 자세 등)
    float torso_ratio = -1.0f;
    bool foreshortened = false;
    if (shoulder_w >= kMinShoulderWidthNorm) {
        spread_ratio = (max_y - min_y) / shoulder_w;
        // 주저앉기 게이트: 몸통까지 단축됐을 때만 인정 (상수 주석 참조)
        torso_ratio = std::hypot(dx, dy) / shoulder_w;
        foreshortened = spread_ratio < ratio_limit &&
                        torso_ratio < kMaxTorsoRatioForeshort;
    }

    const bool lying = by_angle || inverted || foreshortened;
    (void)extent;  // 아래 로그 비활성화 중 미사용 경고 방지 (임계값 튜닝 시 로그와 함께 복원)
    // 로그 정리로 비활성화 — 디버깅 시 해제
    // std::fprintf(stderr,
    //              "[pose] 기울기=%.1f도(기준%.0f%s) 하체veto=%s 반전=%s 스프레드=%.2f(~%s, 기준<%.1f%s) 몸통=%.2f → %s\n",
    //              angle_from_vertical, kLyingAngleDeg,
    //              (angle_from_vertical >= kLyingAngleDeg) ? "✓" : "",
    //              legs_upright ? "다리섬" : "-", inverted ? "✓" : "-",
    //              spread_ratio, extent, ratio_limit, foreshortened ? "✓" : "",
    //              torso_ratio, lying ? "누움" : "서있음");
    return lying;
}

bool PoseEstimator::isLyingDown(const cv::Mat& personCropBgr) const {
    std::array<Keypoint, kNumKeypoints> kp;
    if (!estimate(personCropBgr, kp)) return false;
    return isLyingPose(kp);
}
