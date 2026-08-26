#ifndef APP_ALGORITHMS_HEART_RATE_CALC_H_
#define APP_ALGORITHMS_HEART_RATE_CALC_H_

#include <stdint.h>
#include "max30102.h"

/* 심박 및 생체 신호 상태 정의 */
typedef enum {
    HR_STAT_NONE = 0,       // 미착용 또는 신호 없음
    HR_STAT_ACQUIRING,      // 착용 감지 후 생체 록(Lock) 빌드업 중
    HR_STAT_VALID           // 생체 신호 검증 완료 (정밀 데이터 출력 수행)
} HRState_t;

/* 심박수 모듈 초기화 */
void HeartRateCalc_Init(void);

/**
  * FIFO 블록 단위 생체 신호 처리
  *
  * @param samples   MAX30102 FIFO 한 블록 (RED/IR)
  * @param count     배열 유효 길이
  * @param motion_g  직전 IMU 블록의 움직임 강도(|SVM-1g| 평균, g).
  *                  모션 블랭킹 판단에 쓰인다.
  */
void HeartRateCalc_ProcessBlock(const MAX30102_Data_t *samples, uint16_t count, float motion_g);

void     HeartRateCalc_Reset(void);

/* 맥박으로 확인된 착용 여부
 * 책상·침대는 맥박이 없어 여기서 1 이 될 수 없다. */
uint8_t  HeartRateCalc_IsWorn(void);

/* 광학적 접촉 여부 (IR DC 가 충분한가)
 *
 * AGC 처럼 '맥박 이전 단계' 에서 동작해야 하는 쪽은 반드시 이 함수를 써야 한다.
 * IsWorn() 을 쓰면 AGC 가 맥박을 기다리고 맥박은 AGC 를 기다리는
 * 순환 의존에 빠져 둘 다 영영 시작되지 않는다. */
uint8_t  HeartRateCalc_HasContact(void);

/* AGC 가 LED 전류를 바꾼 직후 호출.
 * 전류 변경은 DC 계단 점프를 만들고, 필터 입장에서는 거대한 AC 과도 응답과
 * 구분되지 않는다. 알려주지 않으면 그 과도분이 맥동으로 오인된다. */
void     HeartRateCalc_NotifyGainChange(void);

/* 과도응답 안정화 구간인가.
 *
 * AGC 는 이 구간에 판단을 미뤄야 한다. 착용 직후 첫 블록은 DC 가 공기에서
 * 조직으로 계단 점프하는 덩어리라, 그 평균으로 전류를 정하면 반드시 틀린다.
 * 틀린 조정은 다음 조정을 부르고, 조정마다 파형 이력에 이음매가 생긴다. */
uint8_t  HeartRateCalc_IsSettling(void);

/* --- [ 상위 레이어 데이터 획득 API ] --- */
uint32_t HeartRateCalc_GetBPM(void);
uint32_t HeartRateCalc_GetSpO2(void);

/* 화면·BLE 표시용 심박. VALID 이 풀려도 마지막 실측값을 최대 30초 유지한다.
 * 낙상 순간에는 충격이 PPG 를 망가뜨려 GetBPM() 이 0 을 돌려주기 때문이다.
 * 값의 신선도를 보장하지 않으므로 판정 로직은 GetBPM() 을 쓸 것. */
uint32_t HeartRateCalc_GetBPMHeld(void);

#endif /* APP_ALGORITHMS_HEART_RATE_CALC_H_ */
