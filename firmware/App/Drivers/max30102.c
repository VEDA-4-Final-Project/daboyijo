#include "max30102.h"
#include <stdio.h>

extern I2C_HandleTypeDef hi2c1;

// 1바이트 읽기. 주소(1B) 전송 후 데이터(1B) 수신
HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return HAL_ERROR;

    // I2C 멤블록 읽기를 통해 레지스터 값 안전하게 수신
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(&hi2c1, MAX30102_I2C_ADDR, reg,
                                                I2C_MEMADD_SIZE_8BIT, val, 1, 100);
    return status;
}

// 1바이트 쓰기
HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val)
{
    // I2C 멤블록 쓰기를 통해 지정 레지스터에 데이터 전송
    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(&hi2c1, MAX30102_I2C_ADDR, reg,
                                                 I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    return status;
}

// 센서 초기화 함수
HAL_StatusTypeDef MAX30102_Init(void)
{
    uint8_t part_id = 0;

    printf("=== MAX30102 Clean Initialization ===\r\n");

    // 1. 칩 ID 확인 및 통신 점검
    if (MAX30102_ReadRegister(MAX30102_REG_PART_ID, &part_id) != HAL_OK) return HAL_ERROR;
    if (part_id != MAX30102_PART_ID_VAL) {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", part_id);
        return HAL_ERROR;
    }

    // 2. 소프트웨어 리셋 수행 및 안정화 대기
    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x40) != HAL_OK) return HAL_ERROR;
    HAL_Delay(50); // 리셋 후 내부 아날로그 회로 안정화 대기

    // 3. 인터럽트 설정 (FIFO 데이터 가득 참(A_FULL) 및 새로운 데이터 수신(PPG_RDY) 활성화)
    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_1, 0xC0) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_2, 0x00) != HAL_OK) return HAL_ERROR;

    // 4. FIFO 포인터 초기화 (기존 잔여 데이터 제거)
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_WR_PTR, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_OVF_COUNTER, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_RD_PTR, 0x00) != HAL_OK) return HAL_ERROR;

    // 5. FIFO 및 모드 설정 (Sample Average: 4, FIFO Roll-Over 활성화, SpO2 Mode 진입)
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_CONF, 0x5F) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x03) != HAL_OK) return HAL_ERROR; // SpO2 모드 (RED + IR 동시 사용)

    // 6. SpO2 파라미터 설정 (ADC 범위: 4096nA, ODR: 100Hz, LED 펄스 폭: 411us / 18비트 해상도)
    if (MAX30102_WriteRegister(MAX30102_REG_SPO2_CONF, 0x27) != HAL_OK) return HAL_ERROR;

    // 7. LED 전류 진폭 설정 (하드웨어 실장 상태에 맞춰 전류 세기 조절, 약 7.2mA 수준)
    if (MAX30102_WriteRegister(MAX30102_REG_LED1_PA, 0x24) != HAL_OK) return HAL_ERROR; // RED LED
    if (MAX30102_WriteRegister(MAX30102_REG_LED2_PA, 0x24) != HAL_OK) return HAL_ERROR; // IR LED

    // 8. 인터럽트 플래그 클리어 (인터럽트 상태 레지스터 읽음으로써 초기 클리어)
    uint8_t dummy = 0;
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_1, &dummy);
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_2, &dummy);

    printf("[ SUCCESS ] MAX30102 Ready!\r\n");
    return HAL_OK;
}

// DMA 데이터 파싱 함수 (I2C DMA로 읽어온 6바이트 패킷을 24비트 정수형으로 가공)
void MAX30102_Parse_DMA_Data(uint8_t *dma_buf, MAX30102_Data_t *sensor_data)
{
    if (dma_buf == NULL || sensor_data == NULL) return;

    // MAX30102 FIFO 데이터 포맷: RED[0:2] (3바이트), IR[3:5] (3바이트)
    // 상위 18비트 유효 데이터 추출을 위해 MSB부터 순차 병합 처리 (상위 6비트는 미사용 공백)
    uint32_t raw_red = ((uint32_t)dma_buf[0] << 16) | ((uint32_t)dma_buf[1] << 8) | dma_buf[2];
    uint32_t raw_ir  = ((uint32_t)dma_buf[3] << 16) | ((uint32_t)dma_buf[4] << 8) | dma_buf[5];

    // 18비트 데이터 마스킹 적용 (최종 가공 정수 매핑)
    sensor_data->red = raw_red & 0x0003FFFF;
    sensor_data->ir  = raw_ir  & 0x0003FFFF;
}
