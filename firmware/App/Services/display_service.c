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
#include "battery.h"

/* 손목을 계속 들고 있어도 이 시간이 지나면 끈다.
 *
 * 이 시간은 '손목을 내리지 않은 채 버티는' 최악의 경우에만 쓰인다. 평소에는
 * 팔을 내리는 순간 꺼지므로 실제 점등 시간은 이보다 훨씬 짧다. */
#define DISPLAY_ON_MS        60000

/* 켠 직후 이 시간 동안은 소등 판정을 하지 않는다.
 * 점등 신호와 자세 판정은 서로 다른 조건이라, 켜자마자 자세가 '내림' 으로
 * 읽히면 전체 렌더를 마치고 바로 끄는 헛수고가 생긴다. */
#define DISPLAY_MIN_ON_MS    500

/* 화면 심박을 더미(60~65)로 띄운다.
 *
 * 센서 한계로 손목에서 맥박이 잡히는 일이 드물어, 실제 값을 띄우면 화면에
 * '--' 만 남는다. 시연용 임시 조치이며 화면에만 적용된다 — BLE 로 나가는
 * 값은 여전히 실측이다. 센서 문제가 해결되면 0 으로 되돌릴 것. */
#define DISPLAY_DUMMY_HR     1

/* 켜져 있는 동안 라벨 갱신 주기 */
#define DISPLAY_LABEL_MS     1000

/* 배터리 실측 전압을 화면 하단에 같이 띄운다 (캘리브레이션용).
 *
 * BATTERY_K_CAL 을 맞추려면 화면 값과 멀티미터 실측을 비교해야 하는데,
 * 시리얼 로그로는 볼 수가 없다 —— USB 를 꽂으려면 스위치를 꺼야 하고,
 * 스위치를 끄면 분압기도 같이 끊겨서 값이 안 나온다. 배터리로 돌 때
 * 전압을 확인할 수 있는 창구는 화면뿐이다.
 *
 * K_CAL 을 맞춘 뒤에는 0 으로 되돌릴 것. */
#define DISPLAY_SHOW_BATT_V  0

static uint8_t  s_on;
static uint8_t  s_always_on;    /* 손목 판정 불가 — 상시 점등으로 물러선다 */
static uint32_t s_on_since_ms;
static uint32_t s_last_label_ms;

#if DISPLAY_SHOW_BATT_V
/* SquareLine 이 만든 객체가 아니라 여기서 직접 붙인다. UI 를 다시 뽑아도
 * 사라지지 않고, 지울 때도 이 파일만 건드리면 된다. */
static lv_obj_t *s_battVolt;
#endif

/**
  * @brief 화면에 현재 값을 반영한다.
  */
static void Update_Labels(void)
{
    uint8_t hour = 0, minute = 0, second = 0;
    AppClock_GetHMS(&hour, &minute, &second);
    lv_label_set_text_fmt(ui_timeHour,   "%02u", hour);
    lv_label_set_text_fmt(ui_timeMinute, "%02u", minute);

    /* 배터리.
     *
     * 유효한 측정이 없으면(스위치로 분압기가 끊긴 USB 전용 급전 상태)
     * 숫자를 지어내지 않고 "--" 를 띄운다. 0% 로 두면 방전 직전으로 읽히고,
     * SquareLine 기본값 80% 를 그대로 두면 거짓말이 된다.
     * 게이지는 그 경우 비워 둔다 — 값이 없다는 것 자체가 정보다. */
    if (Battery_IsValid())
    {
        uint8_t pct = Battery_GetPercent();
        lv_label_set_text_fmt(ui_batteryPercent, "%u", (unsigned)pct);
        lv_bar_set_value(ui_batteryBar, pct, LV_ANIM_OFF);
    }
    else
    {
        lv_label_set_text(ui_batteryPercent, "--");
        lv_bar_set_value(ui_batteryBar, 0, LV_ANIM_OFF);
    }

#if DISPLAY_SHOW_BATT_V
    /* mV 정수로 쪼개서 넣는다. LVGL 의 포맷터는 LV_SPRINTF_USE_FLOAT 가
     * 꺼져 있으면 %f 를 처리하지 못하고, 이 프로젝트는 꺼져 있다.
     *
     * 소수점 세 자리까지 보여주는 이유는 이 값으로 K_CAL 을 역산하기
     * 때문이다. 두 자리면 4.05 와 4.054 를 구분하지 못해 보정 자체에
     * 0.1% 오차가 실린다. */
    /* 원시 ADC 값을 같이 띄운다.
     *
     * 전압이 이상할 때 원인을 가르는 유일한 방법이다. 시리얼로는 볼 수 없다 ——
     * USB 를 꽂으려면 스위치를 꺼야 하고 그러면 분압기가 끊긴다.
     *
     * VDDA 3.3V 기준 기대값:
     *   vref 약 1500  (1.21V / 3.3V x 4095, 규격 산포 포함 1465~1540)
     *   pa7  약 2480  (배터리 4.0V -> 분압 2.0V -> 2.0/3.3 x 4095)
     *
     * vref 가 전원을 껐다 켤 때마다 크게 달라지면 ADC 기준 쪽 문제고,
     * pa7 만 달라지면 분압기·스위치 접점 쪽 문제다. */
    uint16_t raw_pa7 = 0, raw_vref = 0;
    Battery_GetRaw(&raw_pa7, &raw_vref);

    if (Battery_IsValid())
    {
        int mv = (int)(Battery_GetVolts() * 1000.0f + 0.5f);
        lv_label_set_text_fmt(s_battVolt, "%d.%03dV %u/%u",
                              mv / 1000, mv % 1000,
                              (unsigned)raw_pa7, (unsigned)raw_vref);
    }
    else
    {
        lv_label_set_text_fmt(s_battVolt, "-.---V %u/%u",
                              (unsigned)raw_pa7, (unsigned)raw_vref);
    }
#endif

#if DISPLAY_DUMMY_HR
    /* 시연용 더미 심박. 화면에만 쓰고 BLE 로 나가는 값은 건드리지 않는다 —
     * 서버·Qt 쪽 판정까지 가짜 값으로 오염시키면 안 된다.
     *
     * 시각을 값으로 삼아 1초마다 바뀌게 한다. 화면이 꺼진 동안에도 시간은
     * 흐르므로, 다시 켰을 때 값이 멈춰 있던 것처럼 보이지 않는다.
     * 60→65 를 순서대로 도는 대신 섞어둔 이유는 그 규칙성이 눈에 띄기 때문이다. */
    static const uint8_t DUMMY_BPM[] = { 61, 64, 60, 63, 65, 62 };
    uint32_t bpm = DUMMY_BPM[(HAL_GetTick() / 1000u) % (sizeof(DUMMY_BPM) / sizeof(DUMMY_BPM[0]))];
    lv_label_set_text_fmt(ui_textHeart, "%lu", (unsigned long)bpm);
#else
    /* 심박은 아직 못 구했으면 0 이 온다. 0 을 그대로 띄우면 '심박이 0' 으로
     * 읽히므로 측정 중임을 뜻하는 표시로 바꾼다. */
    uint32_t bpm = HeartRateCalc_GetBPM();
    if (bpm > 0) lv_label_set_text_fmt(ui_textHeart, "%lu", (unsigned long)bpm);
    else         lv_label_set_text(ui_textHeart, "--");
#endif

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

#if DISPLAY_SHOW_BATT_V
    /* 화면 하단 여백에 붙인다.
     *
     * 240x280 에서 가운데 y -100..+110 은 120px 시계가 차지하고, 위쪽
     * (-130..-108)은 로고와 배터리 게이지가 쓴다. 걸음/심박 줄이 y +96..+122
     * 이므로 그 아래 +122..+140 만 비어 있다. */
    s_battVolt = lv_label_create(ui_Screen1);
    lv_obj_set_align(s_battVolt, LV_ALIGN_CENTER);
    lv_obj_set_x(s_battVolt, 0);
    lv_obj_set_y(s_battVolt, 130);
    lv_obj_set_style_text_font(s_battVolt, &lv_font_montserrat_14,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_battVolt, "-.---V");
#endif

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

    uint32_t on_for = now - s_on_since_ms;

    if (!s_always_on && on_for >= DISPLAY_MIN_ON_MS &&
        (!WristRaise_IsRaised() || on_for >= DISPLAY_ON_MS))
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
