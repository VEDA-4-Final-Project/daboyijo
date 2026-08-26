#ifndef APP_ALGORITHMS_WRIST_RAISE_H_
#define APP_ALGORITHMS_WRIST_RAISE_H_

#include "bmi270.h"

/* 상태 초기화. 부팅 시 1회. */
void WristRaise_Init(void);

/* IMU FIFO 블록을 그대로 넘긴다 (Process_IMU_Block 안에서 호출).
 * 블록은 0.5초치 50샘플이므로, 판정에 필요한 유지 시간은 블록 안에서
 * 이미 다 흘러 있다 — 블록이 도착한 순간 결론이 난다. */
void WristRaise_ProcessBlock(const BMI270_Data_t *accel, uint16_t count);

/* 지금 손목이 들려 있는가 (히스테리시스 + 디바운스 적용된 상태) */
uint8_t WristRaise_IsRaised(void);

/* '방금 시계를 보려고 들었다' 는 신호를 1회만 반환하고 스스로 지운다.
 * 화면 점등은 반드시 이것으로 해야 한다 — IsRaised() 로 켜면 타임아웃으로
 * 꺼진 직후 팔이 아직 올라가 있다는 이유로 곧바로 다시 켜진다. */
uint8_t WristRaise_ConsumeRaiseEvent(void);

/* 화면 법선이 위를 얼마나 향하는가. 1.0 = 정확히 하늘, 0 = 수직, -1 = 바닥.
 * 축 설정을 맞출 때 쓰는 진단용 값이다. */
float WristRaise_GetFacing(void);

#endif /* APP_ALGORITHMS_WRIST_RAISE_H_ */
