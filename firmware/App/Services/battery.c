/**
  ******************************************************************************
  * @file    battery.c
  * @brief   PA7 분압 + VREFINT 비율 측정으로 배터리 전압과 잔량을 만든다
  ******************************************************************************
  *
  * ── 회로 ────────────────────────────────────────────────────────────
  *
  *   B+ ── SW1 ── R1 100k ──┬── PA7 (ADC1_IN7)
  *                          ├── R2 100k ── GND
  *                          └── C1 0.1uF ── GND
  *
  *   측정 대상은 승압 전 B+ 다. 모듈의 5V 출력을 재면 배터리가 4.2V 든
  *   3.0V 든 항상 5V 라 잔량 정보가 전혀 없다. 승압기의 존재 이유가 곧
  *   배터리 상태를 가리는 것이기 때문이다.
  *
  *   분압기가 스위치를 지나므로, 전원을 끄면 측정 경로도 함께 끊긴다.
  *   의도한 것이다 — 상시 연결이면 MCU 가 꺼진 상태(VDD=0)에서도 PA7 에
  *   2.1V 가 걸려 ESD 다이오드로 전류가 새고, 절대최대정격
  *   V_IN <= VDD + 0.3V 위반이 된다.
  *
  *   대가는 USB 로만 급전할 때 PA7 이 R2 를 통해 GND 로 당겨져 0V 로
  *   읽힌다는 것이다. 그래서 Battery_IsValid() 를 따로 둔다.
  *
  * ── 왜 VREFINT 를 같이 읽는가 ───────────────────────────────────────
  *
  *   ADC 는 절대 전압을 못 잰다. 재는 것은 기준전압 대비 비율이다.
  *
  *       ADC = (Vin / VREF) * 4095
  *
  *   Black Pill 은 VREF+ 핀이 따로 없어 VDDA(보드 3.3V 레일)가 곧
  *   기준전압이고, 그 3.3V 는 온보드 LDO 가 만든다. LDO 오차와 온도
  *   드리프트가 그대로 측정 오차가 된다.
  *
  *   내부 기준 VREFINT(1.21V, ADC1_IN17)를 같은 자로 함께 재서 나누면
  *   VDDA 가 약분되어 식에서 사라진다.
  *
  *       V_bat = (raw_pa7 / raw_vrefint) * 1.21 * 2
  *
  *   이 프로젝트는 승압 모듈이 LDO 입력을 5V 로 고정해 주므로 레일이
  *   붕괴할 일은 없지만, 공짜로 LDO 오차가 사라지므로 쓰지 않을 이유가 없다.
  *
  * ── 샘플링 타임이 480 cycles 인 이유 ────────────────────────────────
  *
  *   두 가지를 동시에 만족시켜야 한다.
  *
  *   1) 소스 임피던스. 충전 경로 저항은 분압 저항의 합이 아니라 병렬값
  *      (테브난 등가)이다. 100k || 100k = 50k.
  *      12비트 정착에 9.7 * (50k + 6k) * 4pF = 2.2us → 24MHz 에서 53 cycles.
  *   2) VREFINT 의 최소 샘플링 시간 10us 요구조건.
  *
  *   ADCCLK 은 APB2 96MHz / 4 = 24MHz. 480+12 cycles = 20.5us 로 둘 다
  *   여유 있게 넘는다. 2)번을 놓치면 VREFINT 값이 틀어져 보정이 통째로
  *   망가지는데, 증상이 "전압이 조금 이상함"이라 찾기 어렵다.
  *
  ******************************************************************************
  */
#include "battery.h"

#include "main.h"
#include "adc.h"

/* 분압비 x VREFINT. 멀티미터로 한 번 재서 보정한다.
 *
 *   K = V_실측 * raw_vrefint / raw_pa7
 *
 * 이 상수 하나가 분압 저항 오차(1%), VREFINT 개체차(±2.5%), LDO 오차를
 * 전부 흡수한다. 보드마다 한 번씩만 하면 되고 결과는 ±1% 이내다.
 * 초기값은 이론값 1.21 * 2. */
#define BATTERY_K_CAL        2.4200f

/* 측정 주기. 배터리 전압은 느린 신호라 1초면 충분하고, 더 자주 읽으면
 * 홀드 커패시터가 퍼가는 전하 때문에 50k 분압에 스위치드 커패시터 부하가
 * 걸려 오히려 낮게 읽힌다. */
#define BATTERY_PERIOD_MS    1000

/* 채널당 오버샘플 횟수. 한 변환이 20.5us 이므로 8x2 = 약 0.33ms 블로킹.
 * 메인 루프가 IWDG 를 물고 도는 구조라 이 이상 늘리지 않는다.
 * 어차피 아래 저역통과 필터가 시간축으로 한 번 더 평균한다. */
#define BATTERY_OVERSAMPLE   8

/* ADC 폴링 타임아웃. 20.5us x 8 = 164us 면 끝나므로 10ms 는 충분한 여유다. */
#define BATTERY_ADC_TIMEOUT  10

/* 이 값보다 낮으면 측정이 유효하지 않다고 본다.
 *
 * 배터리가 물려 있으면 절대 나올 수 없는 값이다(모듈 과방전 보호가 2.9V).
 * 스위치가 분압기를 끊은 상태, 부팅 직후 두 극의 접점 시차, 스위치 채터링이
 * 전부 여기서 걸러진다. 이게 없으면 순간적으로 0% 경고가 뜬다. */
#define BATTERY_VALID_MIN_V  2.5f

/* 저역통과 필터 계수. 화면 갱신이나 LED 점등으로 부하가 출렁이면 내부저항
 * 때문에 전압이 수십 mV 씩 요동친다. 1초 주기에 0.1 이면 시정수 약 10초. */
#define BATTERY_LPF_ALPHA    0.1f

/* 표시값 히스테리시스 [%p].
 *
 * 이만큼 벌어졌을 때만 표시값을 따라 옮긴다. 1%p 차이로 계속 오르내리는
 * 깜빡임만 막는 것이 목적이고, 실제 변화는 오르든 내리든 그대로 따라간다.
 *
 * 처음에는 '단조 감소 강제'(내려갈 때만 갱신, 5%p 이상 뛰면 충전으로 간주)를
 * 썼는데 충전 중에 못 쓸 물건이었다. 오르는 쪽이 막히니 표시가 한동안 얼어
 * 있다가 6%p 씩 계단으로 뛴다 —— 55% 에서 멈춰 있다가 갑자기 80% 가 되는 식.
 * 전압 쪽 저역통과 필터가 이미 부하 출렁임을 걸러주므로 표시단에서 방향까지
 * 막을 이유가 없었다. */
#define BATTERY_PCT_HYST     2

/* 1S 리튬 방전곡선 (무부하 기준).
 *
 * 선형 변환을 쓰면 안 된다. 3.6~3.9V 구간이 거의 평평한데 그 구간이 전체
 * 용량의 60% 라, 직선으로 매핑하면 3.7V 근처에서 퍼센트가 급락해
 * "80% 였는데 갑자기 20%" 가 된다.
 *
 * 0% 를 3.0V 가 아니라 3.30V 로 잡은 것은 승압 모듈의 과방전 보호가
 * 2.9V 에서 걸리기 때문이다. 그 전에 여유를 둔다. */
static const float   V_LUT[] = { 3.30f, 3.55f, 3.68f, 3.74f, 3.77f, 3.79f,
                                 3.82f, 3.87f, 3.93f, 4.00f, 4.10f, 4.20f };
static const uint8_t P_LUT[] = {     0,     5,    10,    20,    30,    40,
                                    50,    60,    70,    80,    90,   100 };
#define LUT_N  (sizeof(P_LUT) / sizeof(P_LUT[0]))

static float    s_volts;            /* 필터링된 전압. 0 이면 아직 유효 측정 없음 */
static uint8_t  s_percent;
static uint8_t  s_valid;
static uint32_t s_last_ms;

static uint16_t s_raw_pa7;          /* 진단용 — 마지막 변환의 채널별 평균 */
static uint16_t s_raw_vref;

/**
  * @brief 한 채널을 오버샘플해서 합을 돌려준다.
  * @retval 0 이면 변환 실패 (호출부에서 이번 주기를 버린다)
  *
  * 매 변환마다 ConfigChannel 을 다시 부르는 이유는 IN7 과 VREFINT 를
  * 번갈아 읽기 때문이다. 스캔 모드로 두 채널을 한 시퀀스에 넣을 수도 있지만,
  * 그러면 DMA 없이는 순서가 꼬이기 쉽고 여기서는 속도가 전혀 아쉽지 않다.
  */
static uint32_t adc_accumulate(uint32_t channel)
{
    ADC_ChannelConfTypeDef cfg = {0};
    uint32_t sum = 0;

    cfg.Channel      = channel;
    cfg.Rank         = 1;
    cfg.SamplingTime = ADC_SAMPLETIME_480CYCLES;
    if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) return 0;

    for (int i = 0; i < BATTERY_OVERSAMPLE; i++)
    {
        if (HAL_ADC_Start(&hadc1) != HAL_OK) return 0;
        if (HAL_ADC_PollForConversion(&hadc1, BATTERY_ADC_TIMEOUT) != HAL_OK)
        {
            HAL_ADC_Stop(&hadc1);
            return 0;
        }
        sum += HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
    }
    return sum;
}

/**
  * @brief 배터리 전압을 한 번 측정한다.
  * @retval 0.0f 이면 측정 실패
  */
static float measure_volts(void)
{
    uint32_t bat = adc_accumulate(ADC_CHANNEL_7);
    uint32_t ref = adc_accumulate(ADC_CHANNEL_VREFINT);

    /* 진단용 원시값은 실패 판정보다 먼저 남긴다 — 실패했을 때 무엇이 0 이었는지가
     * 정확히 알고 싶은 정보다. */
    s_raw_pa7  = (uint16_t)(bat / BATTERY_OVERSAMPLE);
    s_raw_vref = (uint16_t)(ref / BATTERY_OVERSAMPLE);

    if (bat == 0 || ref == 0) return 0.0f;

    /* 오버샘플 횟수는 분자·분모에 똑같이 들어가 약분되므로 나눌 필요가 없다.
     * VDDA 도 여기서 사라진다 — 두 값을 같은 자로 쟀기 때문이다. */
    return BATTERY_K_CAL * (float)bat / (float)ref;
}

/**
  * @brief 전압을 잔량 퍼센트로 바꾼다 (테이블 선형보간)
  */
static uint8_t volts_to_percent(float v)
{
    if (v <= V_LUT[0])         return 0;
    if (v >= V_LUT[LUT_N - 1]) return 100;

    for (uint32_t i = 1; i < LUT_N; i++)
    {
        if (v < V_LUT[i])
        {
            float t = (v - V_LUT[i - 1]) / (V_LUT[i] - V_LUT[i - 1]);
            return (uint8_t)(P_LUT[i - 1] + t * (float)(P_LUT[i] - P_LUT[i - 1]));
        }
    }
    return 100;
}

/**
  * @brief 측정 한 번을 반영한다. 유효하지 않으면 이전 상태를 유지한다.
  */
static void update_once(void)
{
    float v = measure_volts();

    if (v < BATTERY_VALID_MIN_V)
    {
        /* 스위치로 분압기가 끊긴 상태이거나 변환 실패.
         * 이전 값을 지우지 않는다 — 순간적인 접점 불량 때문에 화면의
         * 잔량이 0 으로 깜빡이면 사용자에게는 고장으로 보인다. */
        return;
    }

    if (s_volts == 0.0f) s_volts = v;                               /* 첫 샘플로 시드 */
    else                 s_volts += BATTERY_LPF_ALPHA * (v - s_volts);

    uint8_t p = volts_to_percent(s_volts);

    /* 양방향 히스테리시스. 깜빡임만 막고 방향은 막지 않는다. */
    if (!s_valid ||
        (int)p >= (int)s_percent + BATTERY_PCT_HYST ||
        (int)p + BATTERY_PCT_HYST <= (int)s_percent)
    {
        s_percent = p;
    }
    s_valid = 1;
}

void Battery_Init(void)
{
    s_volts   = 0.0f;
    s_percent = 0;
    s_valid   = 0;

    /* VREFINT 예열용 버리는 변환.
     *
     * HAL_ADC_ConfigChannel 이 ADC_CCR_TSVREFE 를 켜는데, 내부 기준의 기동
     * 시간(약 10us)이 바로 뒤따르는 첫 변환과 겹칠 수 있다. F4 HAL 은
     * 온도센서에만 안정화 지연을 넣고 VREFINT 에는 넣지 않는다.
     * 한 번 버리고 나면 TSVREFE 는 계속 켜진 채라 이후로는 문제없다. */
    (void)adc_accumulate(ADC_CHANNEL_VREFINT);

    update_once();          /* 첫 표시가 0% 에서 기어 올라가지 않도록 시드 */
    s_last_ms = HAL_GetTick();
}

void Battery_Service(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - s_last_ms) < BATTERY_PERIOD_MS) return;
    s_last_ms = now;

    update_once();
}

float Battery_GetVolts(void)
{
    return s_volts;
}

uint8_t Battery_GetPercent(void)
{
    return s_percent;
}

uint8_t Battery_IsValid(void)
{
    return s_valid;
}

void Battery_GetRaw(uint16_t *pa7, uint16_t *vref)
{
    if (pa7)  *pa7  = s_raw_pa7;
    if (vref) *vref = s_raw_vref;
}
