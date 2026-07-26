#include "fall_detection.h"
#include <stdio.h>

#define PRINT_INTERVAL_SAMPLES   50  // 20ms * 50 = 1000ms (1초)

// 모듈 내부 정적 변수 관리
static BMI270_Data_t g_gyro_bias = {0.0f, 0.0f, 0.0f};
static uint16_t g_sample_counter = 0;

void FallDetection_Init(BMI270_Data_t gyro_bias)
{
    // main.c에서 캘리브레이션 완료된 바이아스 값을 내부 변수에 복사
    g_gyro_bias = gyro_bias;
    g_sample_counter = 0;

    printf("[ FALL ] Module Initialized.\r\n");
}

void FallDetection_Update(void)
{
    BMI270_Data_t accel_data = {0.0f, 0.0f, 0.0f};
    BMI270_Data_t gyro_data = {0.0f, 0.0f, 0.0f};

    // 1. BMI270 하드웨어 드라이버로부터 물리량 데이터 읽기
    if (BMI270_Read_Accel(&accel_data) != HAL_OK) return;
    if (BMI270_Read_Gyro(&gyro_data) != HAL_OK) return;

    // 2. 초기화 때 주입받은 정적 자이로 영점 오프셋(Bias) 제거
    gyro_data.x -= g_gyro_bias.x;
    gyro_data.y -= g_gyro_bias.y;
    gyro_data.z -= g_gyro_bias.z;

    // 3. 1초에 한 번씩 가속도/자이로 출력
    g_sample_counter++;
    if (g_sample_counter >= PRINT_INTERVAL_SAMPLES)
    {
    	g_sample_counter = 0;

        // 가속도는 소수점 3자리(g 단위), 자이로는 소수점 1자리(dps 단위)로 시인성 확보
        printf("[IMU DATA] Accel(g): X:%+.3f, Y:%+.3f, Z:%+.3f | Gyro(dps): X:%+.1f, Y:%+.1f, Z:%+.1f\r\n",
               accel_data.x, accel_data.y, accel_data.z,
               gyro_data.x, gyro_data.y, gyro_data.z);
    }
}
