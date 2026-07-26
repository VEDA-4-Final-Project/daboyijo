#ifndef APP_DRIVERS_HEART_RATE_H_
#define APP_DRIVERS_HEART_RATE_H_

#include "main.h"

// --- [레지스터 주소 매크로] ---
#define MAX30102_REG_INT_STATUS_1  0x00		// 인터럽트 상태 1 레지스터
#define MAX30102_REG_FIFO_DATA     0x07		// FIFO 데이터 입력 레지스터
#define MAX30102_REG_MODE_CONFIG   0x09		// 작동 모드(소프트웨어 리셋, 모드 선택) 설정 레지스터
#define MAX30102_REG_CHIP_ID       0xFF		// 칩 ID 확인 레지스터

// --- [기대하는 설정 값] ---
#define MAX30102_CHIP_ID_VAL       0x15
#define MAX30102_I2C_ADDR          0xAE

// --- [통합 데이터 구조체] ---
typedef struct {
    uint32_t red;	// Red LED RAW 데이터 (유효 데이터 18비트)
    uint32_t ir;	// IR LED RAW 데이터 (유효 데이터 18비트)
} MAX30102_Data_t;

// --- [장치 제어 핸들 구조체] ---
typedef struct {
    I2C_HandleTypeDef *hi2c;    // 확장성을 위한 I2C 채널 포인터
} MAX30102_t;

// --- [하위 레벨 통신 함수] ---
HAL_StatusTypeDef MAX30102_ReadRegister(MAX30102_t *dev, uint8_t reg, uint8_t *val);
HAL_StatusTypeDef MAX30102_WriteRegister(MAX30102_t *dev, uint8_t reg, uint8_t val);

// --- [상위 레벨 API 함수] ---
HAL_StatusTypeDef MAX30102_Init(MAX30102_t *dev, I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef MAX30102_Read_FIFO(MAX30102_t *dev, MAX30102_Data_t *data);

#endif
