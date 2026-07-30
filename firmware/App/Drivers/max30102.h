#ifndef APP_DRIVERS_MAX30102_H_
#define APP_DRIVERS_MAX30102_H_

#include "main.h"    // HAL 라이브러리와 핀 이름들을 가져오기 위해 필수

// --- [레지스터 주소 매크로] ---
#define MAX30102_REG_INT_STAT_1      0x00    // 인터럽트 상태 1 레지스터
#define MAX30102_REG_INT_STAT_2      0x01    // 인터럽트 상태 2 레지스터
#define MAX30102_REG_INT_ENABLE_1    0x02    // 인터럽트 활성화 1 레지스터
#define MAX30102_REG_INT_ENABLE_2    0x03    // 인터럽트 활성화 2 레지스터

#define MAX30102_REG_FIFO_WR_PTR     0x04    // FIFO 쓰기 포인터 레지스터
#define MAX30102_REG_OVF_COUNTER     0x05    // 오버플로우 카운터 레지스터
#define MAX30102_REG_FIFO_RD_PTR     0x06    // FIFO 읽기 포인터 레지스터
#define MAX30102_REG_FIFO_DATA       0x07    // FIFO 데이터 레지스터 (데이터 읽기 진입점)

#define MAX30102_REG_FIFO_CONF       0x08    // FIFO 설정 레지스터 (SMP_AVE 등)
#define MAX30102_REG_MODE_CONF       0x09    // 모드 설정 레지스터 (HR/SpO2 모드 등)
#define MAX30102_REG_SPO2_CONF       0x0A    // SpO2 설정 레지스터 (ADC 범위, ODR 등)
#define MAX30102_REG_LED1_PA         0x0C    // LED1 (RED) 전류 펄스 진폭 설정 레지스터
#define MAX30102_REG_LED2_PA         0x0D    // LED2 (IR) 전류 펄스 진폭 설정 레지스터

#define MAX30102_REG_PART_ID         0xFF    // 칩 ID 확인 레지스터

// --- [기대하는 설정 값] ---
#define MAX30102_PART_ID_VAL         0x15    // 칩 ID 기본값
#define MAX30102_I2C_ADDR            (0x57 << 1) // 7비트 주소(0x57)를 좌측 시프트한 8비트 주소

// --- [통합 데이터 구조체] ---
typedef struct {
    uint32_t red;    // RED 광학 센서 값 (심박 계산용 raw data)
    uint32_t ir;     // IR 광학 센서 값 (산소포화도 계산용 raw data)
} MAX30102_Data_t;

// --- [하위 레벨 통신 함수] ---
// HAL_StatusTypeDef: 통신 성공 여부를 반환
HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val);
HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val);

// --- [상위 레벨 API 함수] ---
HAL_StatusTypeDef MAX30102_Init(void);
void MAX30102_Parse_DMA_Data(uint8_t *dma_buf, MAX30102_Data_t *sensor_data);

#endif /* APP_DRIVERS_MAX30102_H_ */
