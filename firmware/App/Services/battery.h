#ifndef APP_SERVICES_BATTERY_H_
#define APP_SERVICES_BATTERY_H_

#include <stdint.h>

/* PA7 분압을 읽어 배터리 전압과 잔량을 만든다.
 *
 * 측정 대상은 승압 전 B+ 다. 모듈의 5V 출력은 배터리가 4.2V 든 3.0V 든
 * 항상 5V 이므로 잔량 정보가 없다. */

/* ADC 를 한 번 읽어 필터를 시드한다. 부팅 시 1회.
 * 이걸 부르지 않으면 첫 표시가 0% 에서 실제값까지 서서히 올라간다. */
void Battery_Init(void);

/* 메인 루프에서 매 바퀴 호출. 주기 제어는 내부에서 한다(1초).
 * 한 번 실제로 측정할 때 약 0.33ms 동안 블로킹된다. */
void Battery_Service(void);

/* 필터링된 전압 [V]. 아직 유효한 측정이 없으면 0.0f. */
float Battery_GetVolts(void);

/* 화면 표시용 잔량 [0..100].
 * 단조 감소가 걸려 있어 충전 중이 아니면 올라가지 않는다. */
uint8_t Battery_GetPercent(void);

/* 유효한 측정을 한 번이라도 했는가.
 * 스위치가 분압기를 끊은 상태(USB 로만 급전)면 계속 0 이다. */
uint8_t Battery_IsValid(void);

/* 진단용 — 마지막 변환의 채널별 평균 원시값.
 *
 * 전압이 이상할 때 어느 쪽이 원인인지 가르는 용도다.
 *   vref 가 흔들린다  -> ADC 기준/VREFINT 문제
 *   pa7 만 흔들린다   -> 분압기·스위치 접점·배선 문제
 *   둘 다 안정한데 전압이 틀리다 -> K_CAL 문제
 *
 * VDDA 3.3V 기준 기대값: vref 약 1500, pa7 약 2480 (배터리 4.0V 일 때). */
void Battery_GetRaw(uint16_t *pa7, uint16_t *vref);

#endif /* APP_SERVICES_BATTERY_H_ */
