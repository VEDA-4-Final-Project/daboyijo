#ifndef APP_ALGORITHMS_HEART_RATE_CALC_H_
#define APP_ALGORITHMS_HEART_RATE_CALC_H_

#include "max30102.h"
#include <stdint.h>

// 심박 및 생체 신호 상태 정의
typedef enum {
    HR_STAT_NONE = 0,   // 측정 전 상태 (또는 손가락 미접촉)
    HR_STAT_ACQUIRING,  // 데이터 수집 및 안정화 중
    HR_STAT_VALID       // 신뢰할 수 있는 BPM/SpO2 연산 완료 상태
} HRState_t;

void HeartRateCalc_Init(void);
void HeartRateCalc_Reset(void);
HRState_t HeartRateCalc_Update(MAX30102_Data_t *sensor_data, float gyro_x, float gyro_y, float gyro_z);

uint32_t HeartRateCalc_GetBPM(void);
uint32_t HeartRateCalc_GetSpO2(void);

#endif /* APP_ALGORITHMS_HEART_RATE_CALC_H_ */
