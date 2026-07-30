#ifndef APP_ALGORITHMS_HEART_RATE_CALC_H_
#define APP_ALGORITHMS_HEART_RATE_CALC_H_

#include <stdint.h>
#include "max30102.h"  // 💡 [핵심] 구조체 중복 정의를 없애고 드라이버 헤더를 직접 참조합니다!

/* --- [ 심박 및 생체 신호 상태 정의 ] --- */
typedef enum {
    HR_STAT_NONE = 0,       // 미착용 또는 신호 없음
    HR_STAT_ACQUIRING,      // 착용 감지 후 생체 록(Lock) 빌드업 중
    HR_STAT_VALID           // 생체 신호 검증 완료 (정밀 데이터 출력 수행)
} HRState_t;

/* --- [ main.c 맞춤형 필수 연동 API ] --- */
void     HeartRateCalc_Init(void);
void     HeartRateCalc_Process_DMA(MAX30102_Data_t *sensor_data, float gyro_x, float gyro_y, float gyro_z);
void     HeartRateCalc_Reset(void);
uint8_t  HeartRateCalc_IsWorn(void);

/* --- [ 상위 레이어 데이터 획득 API ] --- */
uint32_t HeartRateCalc_GetBPM(void);
uint32_t HeartRateCalc_GetSpO2(void);

#endif /* APP_ALGORITHMS_HEART_RATE_CALC_H_ */
