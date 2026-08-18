#ifndef APP_ALGORITHMS_STEP_COUNTER_H_
#define APP_ALGORITHMS_STEP_COUNTER_H_

#include "bmi270.h"

/* 만보기 모듈 초기화 */
void StepCounter_Init(void);

/**
  * FIFO 블록 단위 걸음 검출
  *
  * 착용 여부를 묻지 않는다. 손에 들고 있든 손목에 차든 상시 계수한다.
  * (판단 근거는 step_counter.c 상단 '착용 판정을 쓰지 않는 이유' 참조)
  *
  * @param accel 가속도 배열 (g 단위), BMI270 FIFO 한 블록
  * @param count 배열 유효 길이
  *
  * @return 이 블록에서 새로 인정된 걸음 수 (화면 갱신 판단에 쓸 수 있다)
  */
uint16_t StepCounter_ProcessBlock(const BMI270_Data_t *accel, uint16_t count);

/* 누적 걸음 수 */
uint32_t StepCounter_GetSteps(void);

/**
  * 현재 케이던스 (분당 걸음 수).
  * 리듬이 확정되지 않았거나 걸음이 끊긴 지 오래면 0 을 돌려준다.
  * 표시용이자 임계값 튜닝용 — 실제 케이던스(보통 100~120spm)와 어긋나면
  * 이중 계수 또는 누락을 의심할 수 있다.
  */
uint16_t StepCounter_GetCadence(void);

/* 걸음 수 직접 지정 (자정 리셋 / 재부팅 후 복원용) */
void StepCounter_SetSteps(uint32_t steps);

/* 누적 걸음 수를 포함한 내부 상태 전체 초기화.
 * 중력 추정값은 0 이 아니라 1.0g(정지 상태)로 되돌린다 — 0 에서 시작하면
 * 필터가 수렴하는 0.5초 동안 가짜 걸음이 무더기로 잡힌다. */
void StepCounter_Reset(void);

#endif /* APP_ALGORITHMS_STEP_COUNTER_H_ */
