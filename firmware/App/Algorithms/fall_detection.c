/**
  ******************************************************************************
  * @file    fall_detection.c
  * @brief   BMI270 가속도 블록 기반 낙상 감지 (3단계 백트래킹)
  ******************************************************************************
  *
  * ── 판정 3단계 ──────────────────────────────────────────────────────
  *
  *   1단계  자유낙하   SVM < 0.75g 가 3샘플 이상 연속
  *   2단계  충격       SVM > 2.5g
  *   3단계  정지       충격 후 5초간 움직임이 거의 없음
  *
  *   세 조건이 모두 성립하고 '착용 확정' 상태여야 낙상으로 신고한다.
  *
  * ── 임계값 근거 ─────────────────────────────────────────────────────
  *
  *   자유낙하 임계는 이론값(0.5g)이 아니라 0.75g 다. 사람이 넘어질 때는
  *   팔이 휘둘리고 몸이 가구·팔다리에 부딪히며 감속되어, 손목 최저값이
  *   0.6~0.8g 에 머물고 이론적 자유낙하까지 내려가지 않는다.
  *
  *   ⚠ 가속도 범위가 ±16g 여야 한다. ±2g 로는 낙상 충격(3~9g)이 축마다
  *     포화되어 임계값 비교 자체가 성립하지 않는다. (bmi270.c ACC_RANGE 참조)
  *
  * ── 움직임 지표 ─────────────────────────────────────────────────────
  *
  *   s_block_motion 은 블록 내 SVM '표준편차'다. 평균 |SVM-1| 이 아니다.
  *   가속도계 스케일 오차 때문에 정지 상태에서도 SVM 이 1.04 로 읽히는데,
  *   평균 편차를 쓰면 그 오차가 가짜 바닥값으로 늘 깔려 정지와 움직임을
  *   구분하지 못한다. 표준편차는 상수 오차에 0 이 된다.
  *     정지 0.001~0.003   /   움직임 0.1 이상
  *
  *   다만 표준편차가 지우는 것은 상수 오차뿐이다. 정지 중에도 랜덤하게
  *   흔들리는 센서 노이즈는 그대로 남으며, 위 0.001~0.003 이 그 값이다.
  *   아래 소비처 임계값들은 이 바닥 노이즈 위에서 잡은 것이다.
  *
  *   이 값을 실제로 판정에 쓰는 곳은 두 군데다 (FallDetection_GetBlockMotion):
  *     PPG 모션 블랭킹  MOTION_BLANKING_THRESHOLD_G  0.05   (heart_rate_calc.c)
  *     SpO2 정지 판정   SPO2_MOTION_LIMIT_G          0.008  (heart_rate_calc.c)
  *   임계값을 만지려면 저 두 상수를 봐야 한다. 이 파일의
  *   FALL_MOTION_LIMIT_G(0.25) 는 s_block_motion 이 아니라 샘플별 순간 편차용이라
  *   이름이 비슷할 뿐 완전히 다른 경로다.
  *
  *   ⚠ SVM 은 순수 회전에 눈이 먼다. 손목을 천천히 돌리면 축별 값은 크게 변해도
  *     벡터 크기는 계속 1g 라 s_block_motion 이 거의 0 으로 나온다. 낙상 판정에는
  *     무해하지만, PPG 입장에서는 센서-피부 광 경로가 실제로 틀어지는 상황을
  *     '정지' 로 통과시킨다는 뜻이다. SpO2 가 이유 없이 튀면 여기를 의심할 것.
  *
  ******************************************************************************
  */
#include "fall_detection.h"
#include <stdio.h>
#include <math.h>

/* -----------------------------------------------------------------
 * [낙상 알고리즘 튜닝 파라미터]  — 전부 BMI270 ODR 100Hz 기준 샘플 수
 *
 * 기존 50Hz 스트리밍 방식은 "무중력을 먼저 본 뒤 타임아웃 안에 충격이 오는지"
 * 기다리는 전방(forward) 판정이었다. 자유낙하 구간이 짧으면 통째로 놓친다.
 *
 * 블록 아키텍처에서는 순서를 뒤집는다. 충격을 먼저 포착하고, 그 시점에서
 * 과거 링버퍼를 되짚어(backtracking) 자유낙하가 선행했는지 확인한다.
 * 충격은 놓치기 어려운 큰 신호라 트리거로 훨씬 안정적이다.
 * ----------------------------------------------------------------- */
#define FALL_FREEFALL_THRESHOLD_G     0.75f  // 1단계: 무중력 판정 임계 (0.6~0.8g)
#define FALL_IMPACT_THRESHOLD_G       10.0f   // 2단계: 충격 판정 임계 (10g 초과)

#define FALL_FREEFALL_LOOKBACK        120    // 루프백 범위 (1.2s)
#define FALL_FREEFALL_MIN_SAMPLES     3      // 최소 연속 길이 (30ms)

#define FALL_INACTIVITY_WINDOW        500    // 3단계: 충격 후 총 관찰 구간 (5초)
#define FALL_INACTIVITY_GUARD         50     // 충격 직후 잔여 진동 무시 구간 (0.5초)
#define FALL_MOTION_LIMIT_G           0.25f  // 움직임으로 칠 |SVM-1g| 임계
#define FALL_MOTION_ALLOWED_SAMPLES   150    // 이 이상 움직이면 스스로 회복한 것으로 본다 (1.5초)

/* 되짚기용 SVM 이력 링버퍼. 블록 경계를 넘어 과거를 볼 수 있어야 하므로
 * 반드시 FALL_FREEFALL_LOOKBACK 보다 커야 한다. */
#define SVM_HIST_LEN                  128

/* -----------------------------------------------------------------
 * [알고리즘 내부 상태 정의]
 * ----------------------------------------------------------------- */
typedef enum {
    FALL_STATE_MONITORING,
    FALL_STATE_INACTIVITY_CHECK
} FallAlgState_t;

/* -----------------------------------------------------------------
 * [모듈 내부 정적(static) 변수 관리]
 *
 * 전부 블록 호출 사이에 살아남아야 하는 값들이다. 낙상은 한 블록(0.5초) 안에
 * 끝나지 않고 자유낙하→충격→5초 정지까지 여러 블록에 걸쳐 일어나므로,
 * 상태를 지역 변수로 두면 블록 경계에서 판정이 끊긴다.
 * 카운터 단위는 모두 '샘플 수' 다 (ODR 100Hz → 100 = 1초).
 * ----------------------------------------------------------------- */

/* 상태머신 현재 위치. 충격을 기다리는 중이거나(MONITORING),
 * 충격을 이미 받고 5초 정지를 검증하는 중이거나(INACTIVITY_CHECK) 둘 중 하나다. */
static FallAlgState_t s_alg_state = FALL_STATE_MONITORING;

static float    s_svm_hist[SVM_HIST_LEN];           // 되짚기(backtracking)용 SVM 이력 링버퍼 (Reset()이 1.0g로 채운다. 0.0f는 자유낙하를 뜻하므로 기본값이 되면 안 된다)
static uint16_t s_hist_idx = 0;                     // 링버퍼에서 '다음에 쓸' 칸
static uint32_t s_hist_filled = 0;                  // 링버퍼에 실제로 채워진 샘플 수
static uint32_t s_inactivity_counter = 0;           // 3단계 정지 검증: 충격 이후 흐른 샘플 수
static uint32_t s_motion_active_samples = 0;        // 5초 중 '움직였다'고 센 샘플 수
static float    s_block_motion = 0.0f;              // 직전 블록의 SVM 표준편차

/* main.c에 구현된 하드웨어 송신 인터페이스 로드 */
extern void Send_Fall_Alert_Hardware(void);

/* 링버퍼에 SVM 한 샘플 적재 */
static void hist_push(float svm)
{
    s_svm_hist[s_hist_idx] = svm;
    s_hist_idx = (uint16_t)((s_hist_idx + 1) % SVM_HIST_LEN);
    if (s_hist_filled < SVM_HIST_LEN) s_hist_filled++;
}

/* 자유낙하 구간이 선행했는지 검사 */
static uint8_t backtrack_has_freefall(float *out_min, uint16_t *out_run)
{
    uint32_t depth = (s_hist_filled < FALL_FREEFALL_LOOKBACK)
                   ? s_hist_filled : (uint32_t)FALL_FREEFALL_LOOKBACK;

    uint16_t run = 0;
    uint16_t best_run = 0;
    float    min_svm = 99.0f;       // 되짚기 구간의 최저 SVM

    /* offset 1 = 충격 샘플 자신이므로 2부터 거슬러 올라간다. */
    for (uint32_t back = 2; back <= depth; back++)
    {
        uint16_t idx = (uint16_t)((s_hist_idx + SVM_HIST_LEN - back) % SVM_HIST_LEN);
        float v = s_svm_hist[idx];

        if (v < min_svm) min_svm = v;

        if (v < FALL_FREEFALL_THRESHOLD_G)
        {
            run++;
            if (run > best_run) best_run = run;
        }
        else
        {
            run = 0;   // 연속성이 끊기면 처음부터
        }
    }

    /* 임계값 조정 근거를 남긴다. 실패했을 때 '얼마나 모자랐는지' 를 모르면
     * 임계값을 어느 방향으로 얼마나 옮겨야 할지 알 수 없다. */
    if (out_min) *out_min = min_svm;
    if (out_run) *out_run = best_run;

    return (best_run >= FALL_FREEFALL_MIN_SAMPLES) ? 1 : 0;
}

void FallDetection_Init(void)
{
    FallDetection_Reset();
    printf("[ FALL ] module initialized. (ODR %dHz)\r\n", BMI270_ACC_ODR_HZ);
}

FallState_t FallDetection_ProcessBlock(const BMI270_Data_t *accel, uint16_t count, uint8_t is_worn)
{
    if (accel == NULL || count == 0) return FALL_NONE;

    FallState_t return_state = FALL_NONE;

    /* Welford 온라인 분산 누적기 — 합/제곱합 대신 평균을 샘플마다 갱신한다.
     * 이유는 루프 뒤 s_block_motion 계산부 주석 참조. */
    float w_mean = 0.0f;   // 여기까지의 진행 평균
    float w_m2   = 0.0f;   // 편차 제곱 누적 (= 분산 × 샘플수)

    for (uint16_t i = 0; i < count; i++)
    {
        /* 가속도 벡터 크기(SVM) — 자세와 무관한 스칼라라 낙상 판별의 기준량이 된다. */
        float svm = sqrtf((accel[i].x * accel[i].x) +
                          (accel[i].y * accel[i].y) +
                          (accel[i].z * accel[i].z));

        /* 정지 판정용 순간 편차 — 블록 통계와 달리 샘플 단위로 본다 */
        float deviation = fabsf(svm - 1.0f);

        /* Welford 갱신. 큰 수(1.04)끼리 빼는 일이 없고, 편차만 누적한다.
         * w_m2 는 '갱신 전 편차 × 갱신 후 편차' 라 두 인자의 부호가 항상 같아
         * 구조적으로 음수가 될 수 없다. */
        float w_delta = svm - w_mean;
        w_mean += w_delta / (float)(i + 1);
        w_m2   += w_delta * (svm - w_mean);

        hist_push(svm);

        switch (s_alg_state)
        {
            case FALL_STATE_MONITORING:
                /* [단계 2] 충격 포착 → [단계 1] 과거 자유낙하 되짚기 */
                if (svm > FALL_IMPACT_THRESHOLD_G)
                {
                    float    ff_min = 0.0f;
                    uint16_t ff_run = 0;
                    uint8_t  has_ff = backtrack_has_freefall(&ff_min, &ff_run);

                    if (has_ff)
                    {
                        printf("[ FALL LOG ] 충격 %.2fg + 자유낙하 확인 (최저 %.2fg, %u샘플 연속) → 5초 정지 검증\r\n",
                               (double)svm, (double)ff_min, ff_run);

                        s_alg_state = FALL_STATE_INACTIVITY_CHECK;
                        s_inactivity_counter = 0;
                        s_motion_active_samples = 0;
                    }
                }
                break;

            case FALL_STATE_INACTIVITY_CHECK:
                s_inactivity_counter++;

                /* 충격 직후 가드 타임이 지난 뒤부터 움직임 활성도를 센다. */
                if (s_inactivity_counter > FALL_INACTIVITY_GUARD)
                {
                    if (deviation > FALL_MOTION_LIMIT_G)
                    {
                        s_motion_active_samples++;
                    }
                }

                if (s_inactivity_counter >= FALL_INACTIVITY_WINDOW)
                {
                    if (s_motion_active_samples < FALL_MOTION_ALLOWED_SAMPLES)
                    {
                        if (is_worn)
                        {
                            printf("\r\n[ALERT] FALL DETECTED\r\n\r\n");
                            Send_Fall_Alert_Hardware();
                            return_state = FALL_DETECTED;
                        }
                        else
                        {
                            printf("[ FALL LOG ] 오보 처리: 기기 탈착 상태\r\n");
                        }
                    }
                    else
                    {
                        printf("[ FALL LOG ] 오보 처리: 정상 회복 상태 (움직임 %lu 샘플)\r\n",
                               (unsigned long)s_motion_active_samples);
                    }

                    s_alg_state = FALL_STATE_MONITORING;
                }
                break;

            default:
                s_alg_state = FALL_STATE_MONITORING;
                break;
        }
    }

    /* 블록 내 SVM 표준편차 = 실제 움직임의 양.
     * 모집단 분산(N 으로 나눔) — 소비처 임계값 0.05 / 0.008 과 스케일이 맞다.
     *
     * 누적은 Welford 방식을 쓴다. float32 에서 E[x²]-E[x]² 로 계산하면
     * 두 큰 값의 차라 오차가 참값(약 8e-4)보다 커져 분산이 음수까지 나온다. */
    s_block_motion = sqrtf(w_m2 / (float)count);

    return return_state;
}

float FallDetection_GetBlockMotion(void)
{
    return s_block_motion;
}

void FallDetection_Reset(void)
{
    s_alg_state = FALL_STATE_MONITORING;
    s_hist_idx = 0;
    s_hist_filled = 0;
    s_inactivity_counter = 0;
    s_motion_active_samples = 0;
    s_block_motion = 0.0f;

    for (uint16_t i = 0; i < SVM_HIST_LEN; i++) s_svm_hist[i] = 1.0f;
}
