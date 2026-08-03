#include "bmi270.h"
#include "bmi270_config.h"
#include <stdio.h>

extern SPI_HandleTypeDef hspi2;

/**
  * @brief  BMI270 레지스터 1바이트 읽기 (SPI 프로토콜: 주소 + 더미 + 데이터 = 총 3바이트)
  */
HAL_StatusTypeDef BMI270_ReadRegister(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return HAL_ERROR;

    uint8_t tx_buf[3] = { (uint8_t)(reg | 0x80), 0x00, 0x00 };
    uint8_t rx_buf[3] = { 0 };

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status == HAL_OK)
    {
        *val = rx_buf[2]; // BMI270 SPI 특성상 3번째 바이트에 실제 데이터가 안착함
    }
    return status;
}

/**
  * @brief  BMI270 레지스터 1바이트 쓰기
  */
HAL_StatusTypeDef BMI270_WriteRegister(uint8_t reg, uint8_t val)
{
    uint8_t tx_buf[2] = { (uint8_t)(reg & 0x7F), val };

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi2, tx_buf, 2, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    return status;
}

/**
  * @brief  BMI270 메모리 초기화 및 펌웨어 스트리밍 구성 함수
  */
HAL_StatusTypeDef BMI270_Init(void)
{
    uint8_t chip_id = 0;
    uint8_t internal_stat = 0;

    printf("=== BMI270 Clean Initialization ===\r\n");

    // 1. 칩 ID 확인 및 통신 신뢰성 점검
    if (BMI270_ReadRegister(BMI270_REG_CHIP_ID, &chip_id) != HAL_OK) return HAL_ERROR;
    if (chip_id != BMI270_CHIP_ID_VAL)
    {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", chip_id);
        return HAL_ERROR;
    }

    // 2. 절전 모드 해제 및 구성 데이터 업로드 준비
    if (BMI270_WriteRegister(BMI270_REG_PWR_CONF, 0x00) != HAL_OK) return HAL_ERROR;
    HAL_Delay(2);
    if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x00) != HAL_OK) return HAL_ERROR;
    HAL_Delay(2);

    // 3. 256바이트 블록 단위로 펌웨어 데이터 구성 스트리밍 (총 8192바이트)
    for (uint16_t i = 0; i < 8192; i += 256)
    {
        // BMI270 내부 설정 버스트 주소는 16비트(Word) 단위로 카운트됨
        uint16_t word_addr = i / 2;

        // 주소 지정 레지스터 로드 (하위 4비트 및 상위 8비트 분할 분배)
        if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_0, (uint8_t)(word_addr & 0x0F)) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_1, (uint8_t)((word_addr >> 4) & 0xFF)) != HAL_OK) return HAL_ERROR;

        uint8_t reg_init_data = BMI270_REG_INIT_DATA;
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);

        // 레지스터 쓰기 명령 전송
        if (HAL_SPI_Transmit(&hspi2, &reg_init_data, 1, 10) != HAL_OK)
        {
            HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
            return HAL_ERROR;
        }

        // 💡 [수정] const 한정자 폐기 경고(Discarded-qualifiers) 방지를 위한 깔끔한 데이터 주소 캐스팅 적용
        uint8_t *p_stream_chunk = (uint8_t *)&bmi270_config_file[i];
        if (HAL_SPI_Transmit(&hspi2, p_stream_chunk, 256, 100) != HAL_OK)
        {
            HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
            return HAL_ERROR;
        }

        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
        HAL_Delay(1);
    }

    // 4. 초기화 제어 시퀀스 종료 및 디바이스 가동 대기
    if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x01) != HAL_OK) return HAL_ERROR;
    HAL_Delay(30);

    // 5. ASIC 내부 코어 로드 상태 검증 및 최종 운전 동작 모드 설정
    if (BMI270_ReadRegister(BMI270_REG_INTERNAL_STAT, &internal_stat) != HAL_OK) return HAL_ERROR;
    if (internal_stat == 0x01)
    {
        printf("[ SUCCESS ] BMI270 Ready!\r\n");

        // 가속도계 및 자이로스코프 전원 켜기
        if (BMI270_WriteRegister(BMI270_REG_PWR_CTRL, 0x06) != HAL_OK) return HAL_ERROR;
        HAL_Delay(10);

        // 50Hz 인터럽트 타이밍 동기화를 위한 ODR(50Hz) 및 대역폭 매핑 설정
        if (BMI270_WriteRegister(BMI270_REG_ACC_CONF, 0x87) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_GYR_CONF, 0x88) != HAL_OK) return HAL_ERROR;
        HAL_Delay(10);

        // 하드웨어 풀스케일 범위 지정 (가속도: ±2g, 자이로: ±2000dps)
        if (BMI270_WriteRegister(BMI270_REG_ACC_RANGE, 0x00) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_GYR_RANGE, 0x00) != HAL_OK) return HAL_ERROR;
        HAL_Delay(50);

        return HAL_OK;
    }
    else
    {
        printf("[ FAIL ] Init Failed! Internal Status: 0x%02X\r\n", internal_stat);
        return HAL_ERROR;
    }
}

/**
  * @brief  자이로스코프 정적 영점 오프셋(Bias) 측정 캘리브레이션 함수
  */
HAL_StatusTypeDef BMI270_Calibrate_Gyro(BMI270_Data_t *bias)
{
    if (bias == NULL) return HAL_ERROR;
    printf("[INFO] Calibrating Gyroscope... Keep the device still.\r\n");

    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    const int sample_count = 100;

    // SPI 버스트 읽기 패킷 배열 구성 (명령어 1B + 더미 1B + 데이터 6B = 총 8바이트)
    uint8_t tx_buf[8] = { 0 };
    uint8_t rx_buf[8] = { 0 };
    tx_buf[0] = (uint8_t)(BMI270_REG_GYR_X_LSB | 0x80);

    for (int i = 0; i < sample_count; i++)
    {
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
        HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

        if (status != HAL_OK) return HAL_ERROR;

        // 패킷 매핑 해제 (rx_buf[2]부터 유효 바이트 할당됨)
        int16_t raw_x = (int16_t)(((uint16_t)rx_buf[3] << 8) | rx_buf[2]);
        int16_t raw_y = (int16_t)(((uint16_t)rx_buf[5] << 8) | rx_buf[4]);
        int16_t raw_z = (int16_t)(((uint16_t)rx_buf[7] << 8) | rx_buf[6]);

        sum_x += raw_x;
        sum_y += raw_y;
        sum_z += raw_z;

        HAL_Delay(20); // 시스템 고유 운전 주기(50Hz) 레이트에 매칭
    }

    // ±2000dps 설정 하에서의 LSB 감도 계수 (16.4 LSB/dps) 기반 변환 및 평균 산출
    bias->x = ((float)sum_x / (float)sample_count) / 16.4f;
    bias->y = ((float)sum_y / (float)sample_count) / 16.4f;
    bias->z = ((float)sum_z / (float)sample_count) / 16.4f;

    printf("[SUCCESS] Calibration Done! Bias -> X:%.1f, Y:%.1f, Z:%.1f dps\r\n",
            bias->x, bias->y, bias->z);

    return HAL_OK;
}

/**
  * @brief main.c의 고속 DMA 수신 버퍼 스트리밍 데이터를 가속도/자이로 물리량으로 동시 디코딩
  */
void BMI270_Parse_DMA_Data(uint8_t *dma_buf, BMI270_Data_t *accel, BMI270_Data_t *gyro, BMI270_Data_t *bias)
{
    if (dma_buf == NULL || accel == NULL || gyro == NULL) return;

    // main.c 타이머 트리거를 기점으로 확보된 2바이트 오프셋 포함 데이터 매핑 수행
    int16_t acc_x_raw = (int16_t)(((uint16_t)dma_buf[3]  << 8) | dma_buf[2]);
    int16_t acc_y_raw = (int16_t)(((uint16_t)dma_buf[5]  << 8) | dma_buf[4]);
    int16_t acc_z_raw = (int16_t)(((uint16_t)dma_buf[7]  << 8) | dma_buf[6]);

    int16_t gyr_x_raw = (int16_t)(((uint16_t)dma_buf[9]  << 8) | dma_buf[8]);
    int16_t gyr_y_raw = (int16_t)(((uint16_t)dma_buf[11] << 8) | dma_buf[10]);
    int16_t gyr_z_raw = (int16_t)(((uint16_t)dma_buf[13] << 8) | dma_buf[12]);

    // 물리 단위 스케일링 (가속도 풀스케일 ±2g -> 16384 LSB/g, 자이로 ±2000dps -> 16.4 LSB/dps)
    accel->x = (float)acc_x_raw / 16384.0f;
    accel->y = (float)acc_y_raw / 16384.0f;
    accel->z = (float)acc_z_raw / 16384.0f;

    gyro->x = (float)gyr_x_raw / 16.4f;
    gyro->y = (float)gyr_y_raw / 16.4f;
    gyro->z = (float)gyr_z_raw / 16.4f;

    // 초기 설정 시 측정된 정적 바이어스 제거 (오차 보정)
    if (bias != NULL)
    {
        gyro->x -= bias->x;
        gyro->y -= bias->y;
        gyro->z -= bias->z;
    }
}
