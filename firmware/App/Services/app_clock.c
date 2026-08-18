/**
  ******************************************************************************
  * @file    app_clock.c
  * @brief   소프트웨어 벽시계 — 시각 소스와 나머지 코드 사이의 완충층
  ******************************************************************************
  *
  * ── 왜 별도 모듈인가 ────────────────────────────────────────────────
  *
  *   이 펌웨어에는 시각 개념이 없었다. RTC 는 꺼져 있고 HM-10 은 송신 전용이며
  *   HAL_GetTick() 은 부팅 후 경과 ms 일 뿐이다.
  *
  *   그런데 시계 표시와 만보기 자정 리셋은 둘 다 벽시계를 요구한다.
  *   각자 알아서 시간을 구하게 두면 시각 소스를 바꿀 때 두 군데를 고쳐야 한다.
  *   여기 한 곳으로 모으고, 소스는 AppClock_SetTime() 하나로만 들어오게 한다.
  *
  * ── 현재 시각 소스: 빌드 시각 (임시) ────────────────────────────────
  *
  *   __TIME__ 은 '컴파일한 순간' 이다. 빌드와 플래시에 걸린 시간만큼 이미
  *   뒤처져 있고, 전원을 껐다 켜면 그 시각으로 되돌아간다. 개발 중 시계 UI 와
  *   자정 리셋을 검증하기 위한 발판일 뿐 실사용 값이 아니다.
  *
  *   교체할 때 손댈 곳은 AppClock_Init() 의 씨앗 한 줄뿐이다:
  *     RTC  → HAL_RTC_GetTime() 결과를 AppClock_SetTime() 으로
  *     BLE  → RPi 가 내려준 시각을 수신 콜백에서 AppClock_SetTime() 으로
  *            (주기적으로 받으면 아래 드리프트도 함께 보정된다)
  *
  * ── 정확도 한계 ─────────────────────────────────────────────────────
  *
  *   시간의 출처가 SysTick 이고, SysTick 은 HSE(25MHz 크리스털)에서 나온다.
  *   일반적인 크리스털 오차 ±20ppm 이면 하루 약 2초가 밀린다. 시계 표시로는
  *   충분하지만 며칠 켜두면 눈에 띈다. 외부 시각을 주기적으로 주입하는 것이
  *   근본 해법이다.
  *
  *   ⚠ LOWPOWER_STOP_MODE 를 1 로 바꾸면 이 시계는 멈춘다.
  *     Stop 모드에서는 SysTick 이 정지해 HAL_GetTick() 이 얼어붙기 때문이다.
  *     그때는 시각 유지를 반드시 RTC(별도 LSI/LSE 로 도는)에 넘겨야 한다.
  *
  ******************************************************************************
  */
#include "app_clock.h"
#include "main.h"
#include <stdio.h>

#define SECONDS_PER_DAY   86400U

/* 자정 기준 경과 초. 이 값 하나가 시계의 전부다. */
static uint32_t s_sec_of_day = 0;

/* s_sec_of_day 가 마지막으로 갱신된 시점의 HAL_GetTick() 값.
 * 1초씩 끊어 밀 때마다 딱 그만큼만 전진시켜, 1초 미만의 나머지 ms 를
 * 버리지 않고 다음 회차로 넘긴다. 안 그러면 루프 한 바퀴마다 오차가 쌓인다. */
static uint32_t s_base_tick = 0;

static uint8_t  s_midnight_pending = 0;   // 자정 통과 — 소비되기 전까지 유지
static uint8_t  s_synced = 0;             // 외부 시각으로 맞춰졌는가

/**
  * @brief  __TIME__ ("HH:MM:SS") 를 자정 기준 초로 환산
  *
  * 컴파일러가 문자열로 박아주므로 파싱 실패를 걱정할 필요가 없다.
  */
static uint32_t build_time_seconds(void)
{
    const char *t = __TIME__;

    uint32_t h = (uint32_t)(t[0] - '0') * 10U + (uint32_t)(t[1] - '0');
    uint32_t m = (uint32_t)(t[3] - '0') * 10U + (uint32_t)(t[4] - '0');
    uint32_t s = (uint32_t)(t[6] - '0') * 10U + (uint32_t)(t[7] - '0');

    return (h * 3600U) + (m * 60U) + s;
}

void AppClock_Init(void)
{
    s_sec_of_day = build_time_seconds() % SECONDS_PER_DAY;
    s_base_tick = HAL_GetTick();
    s_midnight_pending = 0;
    s_synced = 0;

    printf("[ CLOCK ] %02u:%02u:%02u 로 시작 (빌드 시각 — 동기화 전)\r\n",
           (unsigned)(s_sec_of_day / 3600U),
           (unsigned)((s_sec_of_day / 60U) % 60U),
           (unsigned)(s_sec_of_day % 60U));
}

void AppClock_SetTime(uint8_t hour, uint8_t minute, uint8_t second)
{
    if (hour > 23 || minute > 59 || second > 59) return;

    /* 시각을 갈아끼울 때는 자정 검출을 하지 않는다.
     * 어긋나 있던 시계를 되돌리는 것뿐인데 그것을 '하루가 지났다' 로 읽으면
     * 동기화할 때마다 걸음 수가 날아간다. */
    s_sec_of_day = ((uint32_t)hour * 3600U) + ((uint32_t)minute * 60U) + second;
    s_base_tick = HAL_GetTick();
    s_synced = 1;
}

void AppClock_Service(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t elapsed = now - s_base_tick;   /* 뺄셈이라 49.7일 랩어라운드에 안전 */

    if (elapsed < 1000U) return;

    uint32_t secs = elapsed / 1000U;
    s_base_tick += secs * 1000U;            /* 나머지 ms 는 남겨 누적 오차를 없앤다 */

    uint32_t prev = s_sec_of_day;
    s_sec_of_day = (s_sec_of_day + secs) % SECONDS_PER_DAY;

    /* 자정 통과 판정 — 초가 되감겼으면 날짜가 바뀐 것이다.
     * secs 가 하루를 통째로 넘는 경우(루프가 그만큼 멎었다면)도 함께 잡는다. */
    if ((secs >= SECONDS_PER_DAY) || (s_sec_of_day < prev))
    {
        s_midnight_pending = 1;
    }
}

uint8_t AppClock_ConsumeMidnight(void)
{
    if (!s_midnight_pending) return 0;

    s_midnight_pending = 0;
    return 1;
}

void AppClock_GetHMS(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (hour)   *hour   = (uint8_t)(s_sec_of_day / 3600U);
    if (minute) *minute = (uint8_t)((s_sec_of_day / 60U) % 60U);
    if (second) *second = (uint8_t)(s_sec_of_day % 60U);
}

uint32_t AppClock_GetSecondsOfDay(void)
{
    return s_sec_of_day;
}

uint8_t AppClock_IsSynced(void)
{
    return s_synced;
}
