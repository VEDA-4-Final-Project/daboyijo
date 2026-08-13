/**
  ******************************************************************************
  * @file    display_service.c
  * @brief   손목을 들었을 때만 화면을 켠다
  ******************************************************************************
  *
  * ── 켜고 끄는 규칙 ──────────────────────────────────────────────────
  *
  *   켠다   손목 들기의 '상승 엣지' 에서만. 상태(IsRaised)로 켜면 타임아웃
  *          직후 팔이 아직 올라가 있다는 이유로 곧바로 다시 켜진다.
  *   끈다   손목을 내렸거나, 켜진 지 DISPLAY_ON_MS 가 지났을 때.
  *          타임아웃으로 꺼진 뒤에는 팔을 한 번 내렸다 올려야 다시 켜진다.
  *
  * ── 꺼져 있는 동안 무엇을 아끼는가 ──────────────────────────────────
  *
  *   백라이트(수십 mA)뿐 아니라 패널도 SLPIN 으로 재우고, lv_timer_handler()
  *   자체를 호출하지 않는다. 화면이 꺼진 상태에서 LVGL 을 돌리는 것은 아무도
  *   보지 않는 그림을 SPI 로 밀어넣는 순수한 낭비다.
  *
  * ── 켜는 순서에 의미가 있다 ─────────────────────────────────────────
  *
  *   SLPOUT → 라벨 갱신 → 전체 무효화 후 즉시 렌더 → 그 다음에 백라이트.
  *   백라이트를 먼저 켜면 이전에 남아있던 프레임이 잠깐 보인다. 시계에서는
  *   '몇 분 전 시각이 번쩍 떴다 바뀌는' 것으로 보인다.
  *
  * ── 반응 지연 ───────────────────────────────────────────────────────
  *
  *   IMU 블록이 0.5초 주기(BMI270_FIFO_BLOCK_FRAMES=50 @100Hz)라 판정은
  *   평균 0.25초 늦는다. 여기에 패널 기상 120ms + 전체 렌더 약 100ms 가
  *   더해져 체감 0.5초 안쪽이다. 더 줄이려면 FIFO 블록을 짧게 해야 하는데,
  *   그 블록 길이는 낙상/보행/PPG 모션 블랭킹이 함께 쓰는 값이라 여기 사정만
  *   으로 건드리지 않는다.
  *
  ******************************************************************************
  */
#include "display_service.h"

#include "main.h"
#include "st7789.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "ui.h"

#include "wrist_raise.h"
#include "app_clock.h"
#include "heart_rate_calc.h"
#include "step_counter.h"

/* 손목을 계속 들고 있어도 이 시간이 지나면 끈다 */
#define DISPLAY_ON_MS        7000

/* 켜져 있는 동안 라벨 갱신 주기 */
#define DISPLAY_LABEL_MS     1000

static uint8_t  s_on;
static uint8_t  s_always_on;    /* 손목 판정 불가 — 상시 점등으로 물러선다 */
static uint32_t s_on_since_ms;
static uint32_t s_last_label_ms;

/**
  * @brief 화면에 현재 값을 반영한다.
  *
  * 배터리(ui_batteryBar / ui_batteryPercent)는 건드리지 않는다 —— 이 보드에는
  * 전압 측정 경로가 없어서 채울 수 있는 진짜 값이 없다. SquareLine 이 넣어둔
  * 기본값이 그대로 보인다.
  */
static void Update_Labels(void)
{
    uint8_t hour = 0, minute = 0, second = 0;
    AppClock_GetHMS(&hour, &minute, &second);
    lv_label_set_text_fmt(ui_timeHour,   "%02u", hour);
    lv_label_set_text_fmt(ui_timeMinute, "%02u", minute);

    /* 심박은 아직 못 구했으면 0 이 온다. 0 을 그대로 띄우면 '심박이 0' 으로
     * 읽히므로 측정 중임을 뜻하는 표시로 바꾼다. */
    uint32_t bpm = HeartRateCalc_GetBPM();
    if (bpm > 0) lv_label_set_text_fmt(ui_textHeart, "%lu", (unsigned long)bpm);
    else         lv_label_set_text(ui_textHeart, "--");

    lv_label_set_text_fmt(ui_textStep, "%lu", (unsigned long)StepCounter_GetSteps());
}

static void Display_PowerOn(void)
{
    ST7789_Sleep(0);                        /* SLPOUT + DISPON (내부 120ms 대기) */

    Update_Labels();
    lv_obj_invalidate(lv_screen_active());  /* 자는 동안 쌓인 무효 영역은 의미가 없다 */
    lv_refr_now(NULL);                      /* 백라이트를 켜기 전에 다 그려둔다 */

    ST7789_Backlight(1);

    s_on = 1;
    s_on_since_ms = HAL_GetTick();
    s_last_label_ms = s_on_since_ms;
}

static void Display_PowerOff(void)
{
    ST7789_Backlight(0);
    ST7789_Sleep(1);
    s_on = 0;
}

void DisplayService_Init(uint8_t wrist_sensor_ok)
{
    s_always_on = (wrist_sensor_ok == 0);

    ST7789_Init();              /* 내부에서 검은색으로 채운다 */
    ST7789_Backlight(0);        /* 손목을 들 때까지 켜지 않는다 */

    lv_init();
    lv_tick_set_cb(HAL_GetTick);  /* ⚠ lv_init 뒤에 와야 한다 — lv_init 이 전역을 초기화한다.
                                   * 이렇게 물려두면 메인 루프에 lv_tick_inc(5) + HAL_Delay(5)
                                   * 같은 인위적 지연을 넣을 필요가 없다. 그 지연은 IMU/PPG
                                   * 블록 처리 주기를 통째로 밀어버린다. */
    lv_port_disp_init();
    ui_init();

    ST7789_Sleep(1);            /* 패널도 재운 채로 출발 */
    s_on = 0;
}

void DisplayService_Service(void)
{
    uint32_t now = HAL_GetTick();

    if (!s_on)
    {
        if (s_always_on || WristRaise_ConsumeRaiseEvent())
        {
            Display_PowerOn();
        }
        return;                 /* 꺼져 있으면 LVGL 은 아예 돌리지 않는다 */
    }

    /* 켜져 있는 동안 들어온 엣지는 여기서 버린다.
     * 남겨두면 타임아웃으로 꺼진 직후 그 묵은 엣지가 소비되어, 팔을 이미
     * 내렸는데도 화면이 한 번 더 켜진다. */
    (void)WristRaise_ConsumeRaiseEvent();

    if (!s_always_on &&
        (!WristRaise_IsRaised() || (now - s_on_since_ms) >= DISPLAY_ON_MS))
    {
        Display_PowerOff();
        return;
    }

    if (now - s_last_label_ms >= DISPLAY_LABEL_MS)
    {
        s_last_label_ms = now;
        Update_Labels();
    }

    lv_timer_handler();
}

uint8_t DisplayService_IsOn(void)
{
    return s_on;
}

void DisplayService_Wake(void)
{
    if (!s_on) Display_PowerOn();
}
