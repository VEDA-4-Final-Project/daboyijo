/*
 * max30102.h — MAX30102 PPG 센서 드라이버 (인터페이스)
 *
 * 레지스터 맵, 동작 파라미터, 공개 API 를 정의한다.
 * 센서는 50Hz SpO2 모드로 돌며 FIFO 에 25샘플(0.5초)이 쌓이면 인터럽트를 올린다.
 */
#ifndef APP_DRIVERS_MAX30102_H_
#define APP_DRIVERS_MAX30102_H_

#include "main.h"
#include <stdint.h>

/* --- [ 레지스터 주소 맵 ] --- */
#define MAX30102_REG_INT_STAT_1      0x00
#define MAX30102_REG_INT_STAT_2      0x01
#define MAX30102_REG_INT_ENABLE_1    0x02
#define MAX30102_REG_INT_ENABLE_2    0x03

#define MAX30102_REG_FIFO_WR_PTR     0x04
#define MAX30102_REG_OVF_COUNTER     0x05
#define MAX30102_REG_FIFO_RD_PTR     0x06
#define MAX30102_REG_FIFO_DATA       0x07

#define MAX30102_REG_FIFO_CONF       0x08
#define MAX30102_REG_MODE_CONF       0x09
#define MAX30102_REG_SPO2_CONF       0x0A
#define MAX30102_REG_LED1_PA         0x0C
#define MAX30102_REG_LED2_PA         0x0D

#define MAX30102_REG_PART_ID         0xFF

/* --- [ 드라이버 고유 상수 ] --- */
#define MAX30102_PART_ID_VAL         0x15
#define MAX30102_I2C_ADDR            (0x57 << 1)

/* --- [ 인터럽트 인에이블 비트 ] --- */
#define MAX30102_INT_A_FULL          0x80   // FIFO Almost Full
#define MAX30102_INT_PPG_RDY         0x40   // 신규 샘플 1개 준비 완료

/* --- [ 센서 동작 모드 ] ---
 *
 * SPO2_CONF = 0x2B : ADC 범위 4096nA(01) | 샘플레이트 200Hz(010) | 펄스폭 411us(11, 18bit)
 * FIFO_CONF = 0x57 : 샘플평균 4개(010)   | 롤오버 ON(1)          | A_FULL = 7
 *
 *   실효 출력 레이트 = 200Hz / 4 = 50Hz
 *   A_FULL 필드는 '인터럽트 시점에 남은 빈 칸 수'다. FIFO 깊이 32 이므로
 *   7 → 25샘플이 쌓였을 때 인터럽트. 25샘플 = 0.5초 = 150바이트.
 *
 * ⚠ 이 50Hz 는 heart_rate_calc.c 의 SAMPLING_FREQ 와 일치해야 한다.
 *   어긋나면 BPM 이 그 비율만큼 통째로 틀어진다. */
#define MAX30102_SPO2_CONF_VAL       0x2B
#define MAX30102_FIFO_CONF_VAL       0x57

/* --- [ LED 전류 / 자동 이득 조절(AGC) ] ---
 *
 * 전류 레지스터값 × 0.2mA 가 실제 구동 전류다. 적정값은 착용 부위와 접촉
 * 상태마다 달라 고정할 수 없으므로, AGC 가 IR DC 를 보고 실시간으로 맞춘다. */

/* 부팅 시작값 12.6mA. 이후 AGC 가 조정한다. */
#define MAX30102_LED_CURRENT         0x3F

/* AGC 가 겨냥하는 목표 DC.
 * 광량이 늘면 AC/DC 비율(PI)은 그대로여도 AC 의 절대 카운트가 커져 SNR 이
 * 좋아진다. 손목처럼 맥동이 작은 부위에서는 이 절대 크기가 검출 성패를 가른다.
 * 18비트 상한(262143)까지 약 9만 카운트의 포화 여유를 남긴 값이다. */
#define MAX30102_DC_TARGET           165000UL

/* AGC 가 재조정을 멈출 조건 (목표치 ±20%).
 *
 * '허용 범위'가 아니라 '손을 뗄 범위'다. 넓게 잡으면 전류가 목표에서 크게
 * 벗어난 채로도 정상 판정이 되어 AGC 가 복귀시키지 못한다. */
#define MAX30102_DC_TARGET_LOW       130000UL
#define MAX30102_DC_TARGET_HIGH      200000UL

/* 전류 조절 범위.
 * 상한이 높으면 접촉이 나쁠 때 전류가 끝까지 밀려 올라가는데, 그러면 신호가
 * 아니라 잡음이 함께 증폭되어 무기물에서도 품질 게이트를 통과한다.
 * 정상 측정 구간이 0x27~0x33 이므로 0x60(19mA)이면 충분하다. */
#define MAX30102_LED_MIN             0x08
#define MAX30102_LED_MAX             0x60

/* 전류 변경 후 다음 판단까지의 대기 시간.
 *
 * ⚠ 블록 주기(500ms)보다 반드시 길어야 한다. 전류를 바꾸면 FIFO 를 비우므로
 *   직후 블록은 계단 응답 덩어리인데, 그 값을 DC 로 읽고 또 조정하면
 *   자기 과도응답을 쫓는 되먹임이 생긴다.
 *   1500ms = 3블록이라 최소 2블록은 깨끗한 신호를 보고 판단한다. */
#define MAX30102_AGC_PERIOD_MS       1500

/* --- [ 블록 수거 파라미터 ] --- */
#define MAX30102_SAMPLE_RATE_HZ      50
#define MAX30102_FIFO_DEPTH          32
#define MAX30102_BLOCK_SAMPLES       25
#define MAX30102_BYTES_PER_SAMPLE    6     // RED 3바이트 + IR 3바이트
#define MAX30102_BLOCK_BYTES         (MAX30102_BLOCK_SAMPLES * MAX30102_BYTES_PER_SAMPLE)

/* FIFO 가 넘칠 때를 대비해 수신 버퍼는 전체 깊이를 감당할 수 있게 잡는다. */
#define MAX30102_MAX_BLOCK_BYTES     (MAX30102_FIFO_DEPTH * MAX30102_BYTES_PER_SAMPLE)

/* --- [ 데이터 구조체 ] --- */
typedef struct {
    uint32_t red;
    uint32_t ir;
} MAX30102_Data_t;

/* --- [ 하위 레벨 통신 API ] --- */
HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val);
HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val);

/* --- [ 상위 인터페이스 API ] --- */

/* 센서 초기화 및 FIFO/인터럽트 구성 */
HAL_StatusTypeDef MAX30102_Init(void);

/* I2C 버스 스캔 진단 (통신 실패 원인 분리용) */
void MAX30102_BusScan(void);

/* 자동 이득 조절. 블록마다 IR DC 평균을 넘겨 호출한다.
 * 블로킹 I2C 쓰기를 수행하므로 메인 루프에서만 호출할 것.
 * @return 전류를 바꿨으면 1 — 호출 측은 신호 파이프라인을 재안정화해야 한다. */
uint8_t MAX30102_AutoGain(uint32_t ir_dc, uint8_t is_worn);
uint8_t MAX30102_GetLedCurrent(void);

/* FIFO 블록을 I2C DMA 로 일괄 수거 시작 */
HAL_StatusTypeDef MAX30102_StartBlockRead_DMA(uint8_t *rx_buf, uint16_t bytes);

/* 수거한 블록을 RED/IR 배열로 디코딩. 반환값 = 유효 샘플 수 */
uint16_t MAX30102_ParseBlock(const uint8_t *rx_buf, uint16_t bytes,
                             MAX30102_Data_t *out, uint16_t max_samples);

#endif /* APP_DRIVERS_MAX30102_H_ */
