#ifndef APP_SERVICES_DISPLAY_SERVICE_H_
#define APP_SERVICES_DISPLAY_SERVICE_H_

#include <stdint.h>

/* ST7789 + LVGL + SquareLine UI 를 세우고, 화면은 꺼진 채로 시작한다.
 * 부팅 시 1회. IWDG 기동 전에 부를 것 (내부 HAL_Delay 합계가 약 0.2초다).
 *
 * @param wrist_sensor_ok  BMI270 사용 가능 여부(g_bmi270_ok).
 *        0 이면 손목 판정 자체가 불가능하므로 화면을 계속 켜 둔다 —
 *        센서 하나가 죽었다고 화면까지 영영 안 켜지면 사용자에게는
 *        기기가 완전히 죽은 것으로 보인다. */
void DisplayService_Init(uint8_t wrist_sensor_ok);

/* 메인 루프에서 매 바퀴 호출. 손목 상태에 따라 켜고 끄고, 켜져 있는 동안만
 * LVGL 을 돌린다. */
void DisplayService_Service(void);

uint8_t DisplayService_IsOn(void);

/* 손목과 무관하게 강제로 켠다 (낙상 알림처럼 반드시 보여야 하는 경우용).
 * 켜진 뒤의 소등 규칙은 평소와 같다 — 타임아웃 또는 손목 내림. */
void DisplayService_Wake(void);

#endif /* APP_SERVICES_DISPLAY_SERVICE_H_ */
