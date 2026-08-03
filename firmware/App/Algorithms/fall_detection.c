#include "fall_detection.h"
#include <stdio.h>
#include <math.h>

/* -----------------------------------------------------------------
 * [디버그 및 로그 출력 설정]
 * ----------------------------------------------------------------- */
#define PRINT_INTERVAL_SAMPLES     50   // 20ms * 50 = 1000ms (1초 정기 로그용)

/* -----------------------------------------------------------------
 * [낙상 알고리즘 튜닝 파라미터]
 * ----------------------------------------------------------------- */
#define FALL_THRESHOLD_FREEFALL     0.4f   // 1단계: 무중력 임계값 (0.4g 이하)
#define FALL_THRESHOLD_IMPACT       2.0f   // 2단계: 충격 임계값 (2.0g 이상)
#define FREEFALL_WINDOW_SAMPLES     25     // 무중력 발생 후 충격 유효 타임아웃 (25샘플 = 500ms)

#define INACTIVITY_WINDOW_SAMPLES   250    // 3단계: 충격 후 총 관찰 시간 (250샘플 = 5초)
#define INACTIVITY_DELAY_SAMPLES    25     // 충격 직후 잔여 진동을 무시할 가드 타임 (25샘플 = 500ms)
#define INACTIVITY_MOTION_LIMIT     45.0f  // 정지 상태 탈출을 판단할 자이로 절대값 합 (45 dps)
#define SUSTAINED_MOTION_THRESHOLD  100    // 지속적인 움직임 인정 기준 샘플 수

/* -----------------------------------------------------------------
 * [알고리즘 내부 상태 정의]
 * ----------------------------------------------------------------- */
typedef enum {
    FALL_STATE_MONITORING,
    FALL_STATE_INACTIVITY_CHECK
} FallAlgState_t;

/* -----------------------------------------------------------------
 * [모듈 내부 정적(static) 변수 관리]
 * ----------------------------------------------------------------- */
static uint16_t g_sample_counter = 0;
static FallAlgState_t s_alg_state = FALL_STATE_MONITORING;
static int32_t s_freefall_timeout_counter = 0;
static uint32_t s_inactivity_counter = 0;
static uint32_t s_motion_active_samples = 0;
static BMI270_Data_t s_gyro_bias = {0}; // 메인에서 넘어온 캘리브레이션 오프셋 보관용

/* main.c에 구현된 하드웨어 DMA 송신 인터페이스 로드 */
extern void Send_Fall_Alert_Hardware(void);

/**
  * @brief 낙상 알고리즘 모듈 초기화
  */
void FallDetection_Init(BMI270_Data_t gyro_bias)
{
	s_gyro_bias = gyro_bias;

	FallDetection_Reset();
    printf("[ FALL ] Module Initialized.\r\n");
}

/**
  * @brief 50Hz 주기로 호출되는 실시간 낙상 감지 핵심 상태 머신
  */
FallState_t FallDetection_Update(BMI270_Data_t *accel_data, BMI270_Data_t *gyro_data, uint8_t is_worn)
{
    // 포인터 예외 처리 보호막
    if (accel_data == NULL || gyro_data == NULL)
    {
        return FALL_NONE;
    }

    FallState_t return_state = FALL_NONE;

    // 1. 가속도 벡터 크기(SVM) 및 자이로 모션 강도(절대값 합) 계산
    float total_accel = sqrtf((accel_data->x * accel_data->x) +
                              (accel_data->y * accel_data->y) +
                              (accel_data->z * accel_data->z));

    float motion_intensity = fabsf(gyro_data->x) + fabsf(gyro_data->y) + fabsf(gyro_data->z);

    // 2. 실시간 낙상 감지 2단계 상태 머신(FSM) 구동
    switch (s_alg_state)
    {
        case FALL_STATE_MONITORING:
            if (s_freefall_timeout_counter > 0)
            {
                s_freefall_timeout_counter--;
            }

            // [단계 1] 무중력(Free Fall) 포착
            if (total_accel < FALL_THRESHOLD_FREEFALL)
            {
                if (s_freefall_timeout_counter == 0)
                {
                    printf("\r\n[ FALL LOG ] >>> 1단계: 무중력 발생! <<<\r\n");
                }
                s_freefall_timeout_counter = FREEFALL_WINDOW_SAMPLES;
            }

            // [단계 2] 강한 충격(Impact) 발생 포착
            if (total_accel > FALL_THRESHOLD_IMPACT)
            {
                // 유효 타임아웃(500ms) 이내에 충격이 발생했는지 확인
                if (s_freefall_timeout_counter > 0)
                {
                    printf("[ FALL LOG ] >>> 2단계: %.1fg 돌파 충격 감지! 5초 상태 검증 돌입... <<<\r\n", total_accel);

                    s_alg_state = FALL_STATE_INACTIVITY_CHECK;
                    s_inactivity_counter = 0;
                    s_motion_active_samples = 0;
                    s_freefall_timeout_counter = 0;
                }
                else
                {
                    printf("[ IMU ALERT ] 단순 강한 충격 발생 (Net: %.2fg\r\n)", total_accel);
                }
            }
            break;

        case FALL_STATE_INACTIVITY_CHECK:
            s_inactivity_counter++;

            // 충격 직후 500ms(가드 타임)가 지난 시점부터 착용자의 잔여 움직임 활성도 누적셈
            if (s_inactivity_counter > INACTIVITY_DELAY_SAMPLES)
            {
                if (motion_intensity > INACTIVITY_MOTION_LIMIT)
                {
                    s_motion_active_samples++;
                }
            }

            // 5초간의 심사가 최종 완료된 시점
            if (s_inactivity_counter >= INACTIVITY_WINDOW_SAMPLES)
            {
                // 움직임 활성 횟수가 기준치 미달 시 = 스스로 일어나지 못하는 낙상 상태로 최종 확정
                if (s_motion_active_samples < SUSTAINED_MOTION_THRESHOLD)
                {
                    // main.c에서 넘겨받은 심박센서 기반 착용 검증 플래그 확인
                    if (is_worn)
                    {
                        printf("\r\n🚨🚨🚨 [ALERT] FALL DETECTED!!! 🚨🚨🚨\r\n\r\n");
                        Send_Fall_Alert_Hardware(); // main.c의 UART DMA 전송 함수 가동
                        return_state = FALL_DETECTED;
                    }
                    else
                    {
                        printf("[ FALL LOG ] >>> 낙상 오보 처리: 기기 탈착(방치) 상태\r\n");
                    }
                }
                else
                {
                	printf("[ FALL LOG ] >>> 낙상 오보 처리: 정상 회복 및 이동 확인 (지속 움직임 누적: %lu samples) <<<\r\n", (unsigned long)s_motion_active_samples);
                }

                // 모니터링 모드로 안전하게 회귀
                s_alg_state = FALL_STATE_MONITORING;
            }
            break;

        default:
            s_alg_state = FALL_STATE_MONITORING;
            break;
    }

    // 3. 1초 다운샘플링 주기 카운터 관리
    g_sample_counter++;
    if (g_sample_counter >= PRINT_INTERVAL_SAMPLES)
    {
        g_sample_counter = 0;
    }

    return return_state;
}

/**
  * @brief 낙상 모듈 상태 강제 리셋
  */
void FallDetection_Reset(void)
{
    g_sample_counter = 0;
    s_alg_state = FALL_STATE_MONITORING;
    s_freefall_timeout_counter = 0;
    s_inactivity_counter = 0;
    s_motion_active_samples = 0;
}
