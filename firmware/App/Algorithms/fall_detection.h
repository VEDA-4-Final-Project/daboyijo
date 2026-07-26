#ifndef ALGORITHMS_FALL_DETECTION_H_
#define ALGORITHMS_FALL_DETECTION_H_

#include "bmi270.h"

void FallDetection_Init(BMI270_Data_t gyro_bias);
void FallDetection_Update(void);

#endif /* FALL_DETECTION_H */
