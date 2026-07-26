#include "heart_rate.h"

// 1바이트 읽기
HAL_StatusTypeDef MAX30102_ReadRegister(MAX30102_t *dev, uint8_t reg, uint8_t *val)
{
    if (dev == NULL || val == NULL) return HAL_ERROR;
    return HAL_I2C_Mem_Read(dev->hi2c, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, val, 1, 100);
}

// 1바이트 쓰기
HAL_StatusTypeDef MAX30102_WriteRegister(MAX30102_t *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL) return HAL_ERROR;
    return HAL_I2C_Mem_Write(dev->hi2c, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

// 센서 초기화 함수
HAL_StatusTypeDef MAX30102_Init(MAX30102_t *dev, I2C_HandleTypeDef *hi2c)
{
    uint8_t chip_id = 0;
    dev->hi2c = hi2c; // 매개변수로 받은 I2C 핸들러 정보를 구조체 가방에 할당

    printf("\r\n=== MAX30102 Clean Initialization ===\r\n");

    // 1. 칩 ID 확인 및 통신 점검
    if (MAX30102_ReadRegister(dev, MAX30102_REG_CHIP_ID, &chip_id) != HAL_OK) return HAL_ERROR;
    if (chip_id != MAX30102_CHIP_ID_VAL) {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", chip_id);
        return HAL_ERROR;
    }

    // 2. 소프트웨어 리셋 수행 및 안정화 대기
    if (MAX30102_WriteRegister(dev, MAX30102_REG_MODE_CONFIG, 0x40) != HAL_OK) return HAL_ERROR;
    HAL_Delay(50);

    // 3. 센서 세부 레지스터 설정 (동작 모드, 전류량, 샘플링 속도 등)
    // 3-1. FIFO 설정: 샘플 평균화 없음(000), FIFO 롤오버 활성화(1) -> 0x10으로 설정하여 50Hz 데이터 속도 보장
    if (MAX30102_WriteRegister(dev, 0x08, 0x10) != HAL_OK) return HAL_ERROR;

    // 3-2. 모드 설정: SpO2 모드 활성화 (Red LED와 IR LED를 교대로 발광)
    if (MAX30102_WriteRegister(dev, MAX30102_REG_MODE_CONFIG, 0x03) != HAL_OK) return HAL_ERROR;

    // 3-3. SpO2 설정: ADC 범위 4096nA, 샘플링 레이트 50Hz, 펄스 폭 411us (18비트 해상도)
    if (MAX30102_WriteRegister(dev, 0x0A, 0x23) != HAL_OK) return HAL_ERROR;

    // 3-4. LED 전류(Pulse Amplitude) 설정: 약 7.2mA로 맞춰 안정적인 투과 광량 확보
    if (MAX30102_WriteRegister(dev, 0x0C, 0x24) != HAL_OK) return HAL_ERROR; // Red
    if (MAX30102_WriteRegister(dev, 0x0D, 0x24) != HAL_OK) return HAL_ERROR; // IR

    // 4. FIFO 잔여 포인터 정돈 및 클리어
    if (MAX30102_WriteRegister(dev, 0x04, 0x00) != HAL_OK) return HAL_ERROR; // WR_PTR
    if (MAX30102_WriteRegister(dev, 0x05, 0x00) != HAL_OK) return HAL_ERROR; // OVF_COUNTER
    if (MAX30102_WriteRegister(dev, 0x06, 0x00) != HAL_OK) return HAL_ERROR; // RD_PTR

    HAL_Delay(50); // 모든 설정이 센서 내부에 완전히 안착할 수 있도록 최종 대기

    printf("[ SUCCESS ] MAX30102 Ready!\r\n");
    return HAL_OK;
}

// FIFO에서 Red/IR 데이터를 한번에 읽기 함수 (Burst Read 최적화 버전)
HAL_StatusTypeDef MAX30102_Read_FIFO(MAX30102_t *dev, MAX30102_Data_t *data)
{
    if (dev == NULL || data == NULL) return HAL_ERROR;

    // 0x04(WR_PTR)부터 0x06(RD_PTR)까지 3바이트를 연속으로 한 번에 읽어 통신 부하 최적화
    uint8_t ptr_buf[3] = {0,};
    if (HAL_I2C_Mem_Read(dev->hi2c, MAX30102_I2C_ADDR, 0x04, I2C_MEMADD_SIZE_8BIT, ptr_buf, 3, 100) != HAL_OK) {
        return HAL_ERROR;
    }

    uint8_t wr_ptr = ptr_buf[0];
    uint8_t rd_ptr = ptr_buf[2];

    // 밀려있는 샘플 수 계산 (MAX30102 FIFO는 최대 32개 항목, 5비트 마스크)
    int samples_available = (int)(wr_ptr - rd_ptr) & 0x1F;

    // 새로 들어온 데이터가 하나도 없다면 BUSY 반환하여 상위 루프 탈출 유도
    if (samples_available == 0) return HAL_BUSY;

    // FIFO 데이터 레지스터(0x07)에서 가장 오래된 샘플 1개(6바이트) 읽기
    uint8_t rx_buf[6] = {0,};
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(dev->hi2c, MAX30102_I2C_ADDR, MAX30102_REG_FIFO_DATA,
                                               I2C_MEMADD_SIZE_8BIT, rx_buf, 6, 100);

    if (status != HAL_OK) return status;

    // 읽어온 샘플 데이터를 구조체에 조립하여 반환 (18비트 데이터 유효 마스킹)
    data->red = ((uint32_t)rx_buf[0] << 16 | (uint32_t)rx_buf[1] << 8 | rx_buf[2]) & 0x03FFFF;
    data->ir  = ((uint32_t)rx_buf[3] << 16 | (uint32_t)rx_buf[4] << 8 | rx_buf[5]) & 0x03FFFF;

    return HAL_OK;
}
