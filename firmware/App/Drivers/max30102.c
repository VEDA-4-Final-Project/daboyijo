/*
 * max30102.c — MAX30102 PPG 센서 드라이버
 *
 * 센서를 50Hz SpO2 모드(RED + IR 동시 측정)로 돌리고, FIFO 에 25샘플이
 * 쌓이면 A_FULL 인터럽트를 올린다. MCU 는 그 시점에 I2C DMA 로 블록을
 * 한 번에 수거한다.
 *
 * 제공 기능
 *   MAX30102_Init()               센서 초기화 및 FIFO/인터럽트 구성
 *   MAX30102_StartBlockRead_DMA() FIFO 블록 수거 시작 (비블로킹)
 *   MAX30102_ParseBlock()         수거한 바이트열 → RED/IR 배열
 *   MAX30102_AutoGain()           IR DC 를 보고 LED 전류 자동 조절
 *   MAX30102_BusScan()            I2C 배선 점검용 주소 스캔
 */
#include "max30102.h"
#include <stdio.h>

extern I2C_HandleTypeDef hi2c1;

HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return HAL_ERROR;
    return HAL_I2C_Mem_Read(&hi2c1, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, val, 1, 100);
}

HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(&hi2c1, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

/* 현재 LED 구동 전류. AutoGain 이 실시간으로 바꾼다. */
static uint8_t s_led_current = MAX30102_LED_CURRENT;

uint8_t MAX30102_GetLedCurrent(void) { return s_led_current; }

/**
  * @brief  IR DC 레벨을 보고 LED 전류를 목표 대역으로 맞춘다.
  * @param  ir_dc    이번 블록의 IR 평균값
  * @param  is_worn  광학적 접촉 여부
  * @retval 1 = 전류를 변경함, 0 = 유지
  *
  * 적정 전류는 착용 부위·피부·압력마다 달라 고정할 수 없다. 낮으면 맥동이
  * 노이즈에 묻히고, 높으면 ADC 가 포화되어 AC 성분이 사라진다.
  *
  * @note  블로킹 I2C 쓰기를 수행한다. 메인 루프에서만 호출할 것.
  */
uint8_t MAX30102_AutoGain(uint32_t ir_dc, uint8_t is_worn)
{
    static uint32_t s_last_adjust_ms = 0;

    /* 미착용 중에는 기본 전류로 되돌리고 대기한다. 공기 중에는 목표 DC 에
     * 도달할 수 없어 전류가 상한까지 올라가고, 그대로 피부에 닿으면 포화된다. */
    if (!is_worn)
    {
        if (s_led_current != MAX30102_LED_CURRENT)
        {
            s_led_current = MAX30102_LED_CURRENT;
            MAX30102_WriteRegister(MAX30102_REG_LED1_PA, s_led_current);
            MAX30102_WriteRegister(MAX30102_REG_LED2_PA, s_led_current);
            s_last_adjust_ms = HAL_GetTick();
            return 1;
        }
        return 0;
    }

    /* 전류 변경 후 DC 가 안정될 때까지 다음 판단을 미룬다 */
    if ((HAL_GetTick() - s_last_adjust_ms) < MAX30102_AGC_PERIOD_MS) return 0;

    if (ir_dc >= MAX30102_DC_TARGET_LOW && ir_dc <= MAX30102_DC_TARGET_HIGH) return 0;
    if (ir_dc == 0) return 0;

    /* 비례 제어 — 광량이 전류에 비례하므로 목표 전류를 한 번에 계산한다.
     *   목표전류 = 현재전류 × (목표DC / 측정DC)
     *
     * 포화 구간(26만 이상)에서는 측정 DC 가 상한에 붙어 실제보다 작게 읽히므로,
     * 실제 값을 1.5배로 가정해 과소 보정을 막는다. */
    uint32_t effective_dc = (ir_dc >= 260000UL) ? (ir_dc * 3U / 2U) : ir_dc;

    uint32_t est = (uint32_t)(((uint64_t)s_led_current * MAX30102_DC_TARGET) / effective_dc);

    if (est < MAX30102_LED_MIN) est = MAX30102_LED_MIN;
    if (est > MAX30102_LED_MAX) est = MAX30102_LED_MAX;

    uint8_t next = (uint8_t)est;
    if (next == s_led_current) return 0;

    s_led_current = next;
    MAX30102_WriteRegister(MAX30102_REG_LED1_PA, s_led_current);
    MAX30102_WriteRegister(MAX30102_REG_LED2_PA, s_led_current);

    /* FIFO 를 비운다. 남아있는 샘플은 이전 전류로 찍힌 것이라, 그대로 두면
     * 다음 블록에 옛 전류와 새 전류가 섞여 경계에 큰 계단이 생긴다. */
    MAX30102_WriteRegister(MAX30102_REG_FIFO_WR_PTR, 0x00);
    MAX30102_WriteRegister(MAX30102_REG_OVF_COUNTER, 0x00);
    MAX30102_WriteRegister(MAX30102_REG_FIFO_RD_PTR, 0x00);

    s_last_adjust_ms = HAL_GetTick();
    return 1;
}

/**
  * @brief  I2C 버스에 응답하는 장치 주소를 훑어 출력한다.
  *
  * 통신 실패 시 배선/전원 문제인지 주소 불일치인지 가르는 데 쓴다.
  */
void MAX30102_BusScan(void)
{
    uint8_t found = 0;

    printf("[ I2C SCAN ] 응답 주소: ");
    for (uint8_t addr = 0x08; addr < 0x78; addr++)
    {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 2, 20) == HAL_OK)
        {
            printf("0x%02X ", addr);
            found++;
        }
    }

    if (found == 0)
    {
        printf("없음 → SDA/SCL 배선, 센서 전원(VIN/3V3), 풀업 저항을 확인하세요");
    }
    printf("\r\n");
}

/**
  * @brief  센서를 리셋하고 50Hz SpO2 모드 + A_FULL 인터럽트로 구성한다.
  */
HAL_StatusTypeDef MAX30102_Init(void)
{
    uint8_t part_id = 0;
    printf("=== MAX30102 Clean Initialization ===\r\n");

    /* 주소만 두드리는 가벼운 프로브로 슬레이브가 깨어날 때까지 기다린다.
     * 버스 복구 직후 첫 트랜잭션은 NACK 되는 경우가 있다. */
    if (HAL_I2C_IsDeviceReady(&hi2c1, MAX30102_I2C_ADDR, 5, 100) != HAL_OK) {
        printf("[ FAIL ] I2C 응답 없음 (주소 0x%02X, NACK/타임아웃)\r\n", MAX30102_I2C_ADDR >> 1);
        MAX30102_BusScan();
        return HAL_ERROR;
    }

    if (MAX30102_ReadRegister(MAX30102_REG_PART_ID, &part_id) != HAL_OK) {
        printf("[ FAIL ] PART_ID 판독 실패 (주소는 응답하나 레지스터 접근 불가)\r\n");
        return HAL_ERROR;
    }
    if (part_id != MAX30102_PART_ID_VAL) {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", part_id);
        return HAL_ERROR;
    }

    /* 소프트 리셋 */
    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x40) != HAL_OK) return HAL_ERROR;
    HAL_Delay(50);

    /* A_FULL 만 사용한다. 샘플 단위 PPG_RDY 는 쓰지 않으므로 MCU 는
     * FIFO 가 찰 때(0.5초)만 깨어난다. */
    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_1, MAX30102_INT_A_FULL) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_2, 0x00) != HAL_OK) return HAL_ERROR;

    /* FIFO 포인터 초기화 */
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_WR_PTR, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_OVF_COUNTER, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_RD_PTR, 0x00) != HAL_OK) return HAL_ERROR;

    /* FIFO 구성 → SpO2 모드 진입 → 샘플링/해상도 설정 */
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_CONF, MAX30102_FIFO_CONF_VAL) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x03) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_SPO2_CONF, MAX30102_SPO2_CONF_VAL) != HAL_OK) return HAL_ERROR;

    s_led_current = MAX30102_LED_CURRENT;
    if (MAX30102_WriteRegister(MAX30102_REG_LED1_PA, s_led_current) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_LED2_PA, s_led_current) != HAL_OK) return HAL_ERROR;

    /* 잔류 인터럽트 플래그를 읽어 해제한다 */
    uint8_t dummy = 0;
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_1, &dummy);
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_2, &dummy);

    printf("[ SUCCESS ] MAX30102 Ready!\r\n");
    return HAL_OK;
}

/**
  * @brief  FIFO 블록을 I2C DMA 로 일괄 수거 시작 (비블로킹)
  *
  * FIFO_DATA 를 연속 판독하면 센서가 RD_PTR 을 자동 전진시키고 A_FULL
  * 플래그도 함께 해제한다. 별도의 상태 레지스터 클리어가 필요 없다.
  */
HAL_StatusTypeDef MAX30102_StartBlockRead_DMA(uint8_t *rx_buf, uint16_t bytes)
{
    if (rx_buf == NULL || bytes == 0) return HAL_ERROR;
    if (bytes > MAX30102_MAX_BLOCK_BYTES) return HAL_ERROR;

    return HAL_I2C_Mem_Read_DMA(&hi2c1, MAX30102_I2C_ADDR, MAX30102_REG_FIFO_DATA,
                                I2C_MEMADD_SIZE_8BIT, rx_buf, bytes);
}

/**
  * @brief  수거한 FIFO 블록을 RED/IR 배열로 디코딩
  * @return 해석된 샘플 수
  *
  * 샘플당 6바이트 = RED 3바이트(MSB first) + IR 3바이트, 각 18비트 유효.
  */
uint16_t MAX30102_ParseBlock(const uint8_t *rx_buf, uint16_t bytes,
                             MAX30102_Data_t *out, uint16_t max_samples)
{
    if (rx_buf == NULL || out == NULL) return 0;

    uint16_t samples = bytes / MAX30102_BYTES_PER_SAMPLE;
    if (samples > max_samples) samples = max_samples;

    for (uint16_t i = 0; i < samples; i++)
    {
        const uint8_t *s = rx_buf + (i * MAX30102_BYTES_PER_SAMPLE);

        uint32_t raw_red = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
        uint32_t raw_ir  = ((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5];

        out[i].red = raw_red & 0x0003FFFF;
        out[i].ir  = raw_ir  & 0x0003FFFF;
    }

    return samples;
}
