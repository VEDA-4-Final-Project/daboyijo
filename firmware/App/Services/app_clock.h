#ifndef APP_SERVICES_APP_CLOCK_H_
#define APP_SERVICES_APP_CLOCK_H_

#include <stdint.h>

/**
  * 벽시계 초기화. 빌드 시각을 씨앗으로 심는다 (오차 수 분 — 개발용).
  * 정확한 시각은 AppClock_SetTime() 으로 나중에 덮어쓰면 된다.
  */
void AppClock_Init(void);

/**
  * 외부 시각 주입. RTC 판독값이든 BLE 로 받은 값이든 여기로 들어온다.
  * 이 함수가 시각 소스와 나머지 코드를 갈라놓는 유일한 접점이다 —
  * 소스를 바꿔도 이 함수 호출부만 바뀐다.
  */
void AppClock_SetTime(uint8_t hour, uint8_t minute, uint8_t second);

/**
  * 시각 진행. 메인 루프에서 매 바퀴 호출한다.
  * 1초가 지날 때마다 내부 시각을 밀고, 자정을 넘으면 플래그를 세운다.
  */
void AppClock_Service(void);

/* 자정을 넘었는가. 넘었으면 1 을 돌려주고 플래그를 내린다(1회성).
 * 놓치면 다음 자정까지 기회가 없으므로 매 루프 확인할 것. */
uint8_t AppClock_ConsumeMidnight(void);

/* 현재 시각 (표시용) */
void AppClock_GetHMS(uint8_t *hour, uint8_t *minute, uint8_t *second);

/* 자정 기준 경과 초 (0 ~ 86399) */
uint32_t AppClock_GetSecondsOfDay(void);

/* 외부 시각으로 맞춰졌는가.
 * 0 이면 아직 빌드 시각 추정치다 — 화면에 표시할 때 이 상태를 구분해주면
 * 사용자가 '시계가 틀렸다' 와 '아직 동기화 전이다' 를 헷갈리지 않는다. */
uint8_t AppClock_IsSynced(void);

#endif /* APP_SERVICES_APP_CLOCK_H_ */
