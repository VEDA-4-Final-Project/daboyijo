#ifndef APP_ALGORITHMS_FALL_DETECTION_H_
#define APP_ALGORITHMS_FALL_DETECTION_H_

#include "bmi270.h"

// 낙상 상태 정의
typedef enum {
	FALL_NONE = 0,      // 정상 상태
	FALL_IMPACT,        // 큰 충격 감지 (1단계)
	FALL_DETECTED		// 최종 낙상 확정 (충격 후 무동작)
} FallState_t;

void FallDetection_Init(BMI270_Data_t gyro_bias);
FallState_t FallDetection_Update(uint8_t *dma_buf);
void FallDetection_Reset(void);

#endif /* FALL_DETECTION_H */
