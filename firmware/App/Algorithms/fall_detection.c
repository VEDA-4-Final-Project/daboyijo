#include "fall_detection.h"
#include <stdio.h>
#include <math.h>

#define PRINT_INTERVAL_SAMPLES     50   // 20ms * 50 = 1000ms (1초 정기 로그용)

// -----------------------------------------------------------------
// [낙상 고도화 알고리즘 디파인]
// -----------------------------------------------------------------
#define FALL_THRESHOLD_IMPACT       2.0f   // 2단계: 충격 임계값 (2.0g)
#define FALL_THRESHOLD_FREEFALL     0.4f   // 1단계: 무중력 임계값 (0.4g 이하로 떨어질 때)
#define FREEFALL_WINDOW_SAMPLES     25     // 무중력 발생 후 충격이 올 때까지의 유효 시간 (25샘플 = 500ms)
#define INACTIVITY_WINDOW_SAMPLES   100    // 3단계: 충격 후 정지 상태를 관찰할 시간 (100샘플 = 2초)
#define INACTIVITY_MOTION_LIMIT     35.0f  // 정지 상태로 인정할 최대 자이로 합 (센서 노이즈 및 미세 떨림 무시)

// 알고리즘 내부 상태 정의
typedef enum {
    FALL_STATE_MONITORING,          // 평상시 감시 상태
    FALL_STATE_INACTIVITY_CHECK     // 충격 감지 후, 사후 정지 상태 확인 중
} FallAlgState_t;

// 모듈 내부 정적 변수 관리
static BMI270_Data_t g_gyro_bias = {0.0f, 0.0f, 0.0f};
static uint16_t g_sample_counter = 0;

// 고도화용 상태 변수들
static FallAlgState_t s_alg_state = FALL_STATE_MONITORING;
static int32_t s_freefall_timeout_counter = 0;
static uint32_t s_inactivity_counter = 0;
static uint8_t s_motion_detected_during_inactivity = 0;

void FallDetection_Init(BMI270_Data_t gyro_bias)
{
    g_gyro_bias = gyro_bias;
    g_sample_counter = 0;

    // 상태 초기화
    s_alg_state = FALL_STATE_MONITORING;
    s_freefall_timeout_counter = 0;
    s_inactivity_counter = 0;
    s_motion_detected_during_inactivity = 0;

    printf("[ FALL ] Advanced Module Initialized (Threshold: 3.0g).\r\n");
}

FallState_t FallDetection_Update(uint8_t *dma_buf)
{
    if (dma_buf == NULL) return FALL_NONE;

    BMI270_Data_t accel_data = {0.0f, 0.0f, 0.0f};
    BMI270_Data_t gyro_data = {0.0f, 0.0f, 0.0f};
    FallState_t return_state = FALL_NONE;

    // 1. 드라이버에서 DMA 데이터 파싱
    BMI270_Parse_DMA_Data(dma_buf, &accel_data, &gyro_data, &g_gyro_bias);

    // 2. 가속도 크기(SVM) 및 자이로 모션 강도 계산
    float total_accel = sqrtf(accel_data.x * accel_data.x +
                              accel_data.y * accel_data.y +
                              accel_data.z * accel_data.z);
    float motion_intensity = fabs(gyro_data.x) + fabs(gyro_data.y) + fabs(gyro_data.z);

    // -----------------------------------------------------------------
    // [실시간 낙상 감지 상태 머신]
    // -----------------------------------------------------------------
    switch (s_alg_state)
    {
        case FALL_STATE_MONITORING:
            // 무중력 타이머 차감 (500ms 유효 타임아웃용)
            if (s_freefall_timeout_counter > 0) {
                s_freefall_timeout_counter--;
            }

            // [단계 1] 무중력(Free Fall) 징후 포착
            if (total_accel < FALL_THRESHOLD_FREEFALL) {
                if (s_freefall_timeout_counter == 0) {
                    printf("\r\n[ FALL LOG ] >>> 1단계: 무중력(Free Fall) 발생 포착! <<<\r\n");
                }
                s_freefall_timeout_counter = FREEFALL_WINDOW_SAMPLES; // 500ms 충격 대기 타이머 시작
            }

            // [단계 2] 강한 충격(Impact) 발생 포착
            if (total_accel > FALL_THRESHOLD_IMPACT) {
                // 최근 500ms 이내에 무중력(몸이 뜨는 현상)이 먼저 발생했었는지 확인
                if (s_freefall_timeout_counter > 0) {
                    printf("[ FALL LOG ] >>> 2단계: 2.0g 돌파 충격 감지 (Net: %.2fg)! 사후 정지 상태 모니터링 돌입... <<<\r\n", total_accel);

                    // 정지 상태 검사 모드로 전환
                    s_alg_state = FALL_STATE_INACTIVITY_CHECK;
                    s_inactivity_counter = 0;
                    s_motion_detected_during_inactivity = 0;
                    s_freefall_timeout_counter = 0; // 사용 완료로 리셋
                } else {
                    // 무중력 없이 그냥 충격만 온 경우 (예: 손뼉 치기, 벽 노크)
                    printf("[ IMU ALERT ] 단순 강한 충격 발생 (Net: %.2fg, 무중력 없음 -> 패스)\r\n", total_accel);
                }
            }
            break;

        case FALL_STATE_INACTIVITY_CHECK:
            s_inactivity_counter++;

            // 2초간 모니터링하는 중에 움직임 제한치를 넘는 회복 동작이 감지되면 마킹
            if (motion_intensity > INACTIVITY_MOTION_LIMIT) {
                s_motion_detected_during_inactivity = 1;
            }

            // 2초(100샘플) 관찰이 끝난 시점
            if (s_inactivity_counter >= INACTIVITY_WINDOW_SAMPLES) {
                if (s_motion_detected_during_inactivity == 0) {
                    // [단계 3 최종 통과] 충격 후 완전히 뻗어서 움직임이 없음 -> 낙상 확정!
                    printf("\r\n[ !!! CRITICAL !!! ] >>> 3단계 완료: 낙상(Fall) 최종 감지!!! 사용자가 움직이지 못함 <<\r\n\r\n");
                    return_state = FALL_DETECTED; // 상위 App(main.c)단에 위험 상태 반환 (이늄에 추가 필요)
                } else {
                    // 충격은 컸으나 바로 짚고 일어났거나 움직임이 발생함 -> 낙상 취소
                    printf("[ FALL LOG ] >>> 낙상 오보 처리: 충격 후 사용자 움직임(회복) 감지됨. <<<\r\n");
                }

                // 다시 일반 모니터링 상태로 복귀
                s_alg_state = FALL_STATE_MONITORING;
            }
            break;
    }

    // 3. 기존 루틴: 1초에 한 번씩 정기 데이터 로그 출력
    g_sample_counter++;
    if (g_sample_counter >= PRINT_INTERVAL_SAMPLES)
    {
        g_sample_counter = 0;
        printf("[IMU DATA] Accel: X:%+.3f, Y:%+.3f, Z:%+.3f [Total: %.3fg] | Gyro(dps): X:%+.1f, Y:%+.1f, Z:%+.1f\r\n",
               accel_data.x, accel_data.y, accel_data.z, total_accel,
               gyro_data.x, gyro_data.y, gyro_data.z);
    }

    return return_state;
}

void FallDetection_Reset(void)
{
    g_sample_counter = 0;
    s_alg_state = FALL_STATE_MONITORING;
    s_freefall_timeout_counter = 0;
}
