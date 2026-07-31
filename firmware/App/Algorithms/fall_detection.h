#ifndef APP_ALGORITHMS_FALL_DETECTION_H_
#define APP_ALGORITHMS_FALL_DETECTION_H_

#include "bmi270.h"

// 낙상 상태 정의
typedef enum {
    FALL_NONE = 0,      // 정상 상태
    FALL_DETECTED       // 낙상 확정
} FallState_t;

void FallDetection_Init(BMI270_Data_t gyro_bias);
FallState_t FallDetection_Update(BMI270_Data_t *accel_data, BMI270_Data_t *gyro_data, uint8_t is_worn);
void FallDetection_Reset(void);

#endif /* APP_ALGORITHMS_FALL_DETECTION_H_ */
