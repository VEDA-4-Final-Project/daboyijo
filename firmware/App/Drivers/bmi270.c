#include "bmi270.h"
#include "bmi270_config.h"

extern SPI_HandleTypeDef hspi2;

// 1바이트 읽기. 주소(1B) + 더미(1B)+ 데이터(1B)
HAL_StatusTypeDef BMI270_ReadRegister(uint8_t reg, uint8_t *val)
{
	if (val == NULL) return HAL_ERROR;

    uint8_t tx_buf[3] = {reg | 0x80, 0x00, 0x00};
    uint8_t rx_buf[3] = {0,};

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status == HAL_OK) {
            *val = rx_buf[2];
        }
	return status;
}

// 1바이트 쓰기
HAL_StatusTypeDef BMI270_WriteRegister(uint8_t reg, uint8_t val)
{
    uint8_t tx_buf[2] = {reg & 0x7F, val};

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi2, tx_buf, 2, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    return status;
}

// 센서 초기화 함수
HAL_StatusTypeDef BMI270_Init(void)
{
	uint8_t chip_id = 0;
	uint8_t internal_stat = 0;

    printf("\r\n=== BMI270 Clean Initialization ===\r\n");

    // 1. 칩 ID 확인 및 통신 점검
    if (BMI270_ReadRegister(BMI270_REG_CHIP_ID, &chip_id) != HAL_OK) return HAL_ERROR;
	if (chip_id != BMI270_CHIP_ID_VAL) {
		printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", chip_id);
		return HAL_ERROR;
	}

    // 2. 절전 모드 해제 및 업로드 준비
	if (BMI270_WriteRegister(BMI270_REG_PWR_CONF, 0x00) != HAL_OK) return HAL_ERROR;
	HAL_Delay(2);
	if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x00) != HAL_OK) return HAL_ERROR;
	HAL_Delay(2);

    // 3. 256바이트씩 펌웨어 업로드 스트리밍
    for (uint16_t i = 0; i < 8192; i += 256)
    {
    	// BMI270 메모리는 2바이트 단위
		uint16_t word_addr = i / 2;

		// 주소 지정 레지스터에 주소 넣기 (4096 = 12비트. 하위 4비트 + 상위 8비트)
		if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_0, word_addr & 0x0F) != HAL_OK) return HAL_ERROR;
		if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_1, (word_addr >> 4) & 0xFF) != HAL_OK) return HAL_ERROR;

		// CS 내리고 주소(1B) 쏜 후, 곧바로 데이터(256B) 연속 스트리밍
		uint8_t reg_init_data = BMI270_REG_INIT_DATA;
		HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);

		if (HAL_SPI_Transmit(&hspi2, &reg_init_data, 1, 10) != HAL_OK) {
			HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
			return HAL_ERROR;
		}
		if (HAL_SPI_Transmit(&hspi2, (uint8_t*)&bmi270_config_file[i], 256, 100) != HAL_OK) {
			HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
			return HAL_ERROR;
		}

		HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
		HAL_Delay(1);
    }

    // 4. 초기화 완료 선언 및 안정화 대기
    if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x01) != HAL_OK) return HAL_ERROR;
	HAL_Delay(30);

    // 5. 상태 검증 및 가속도계 활성화
	if (BMI270_ReadRegister(BMI270_REG_INTERNAL_STAT, &internal_stat) != HAL_OK) return HAL_ERROR;
	if (internal_stat == 0x01) {
		printf("[ SUCCESS ] BMI270 Ready!\r\n");

		if (BMI270_WriteRegister(BMI270_REG_PWR_CTRL, 0x06) != HAL_OK) return HAL_ERROR;
		HAL_Delay(10);

		// ODR 및 대역폭 설정 (동기화 모드 유지)
		if (BMI270_WriteRegister(BMI270_REG_ACC_CONF, 0xA8) != HAL_OK) return HAL_ERROR;
		if (BMI270_WriteRegister(BMI270_REG_GYR_CONF, 0xA9) != HAL_OK) return HAL_ERROR;
		HAL_Delay(10);

		// 측정 범위 설정 (가속도: ±2g, 자이로: ±2000dps)
		if (BMI270_WriteRegister(BMI270_REG_ACC_RANGE, 0x00) != HAL_OK) return HAL_ERROR;
		if (BMI270_WriteRegister(BMI270_REG_GYR_RANGE, 0x00) != HAL_OK) return HAL_ERROR;
		HAL_Delay(50);

		return HAL_OK;
    }
    else {
        printf("[ FAIL ] Init Failed!\r\n");
        return HAL_ERROR;
    }
}

// 자이로 영점 오프셋(Bias) 보정
HAL_StatusTypeDef BMI270_Calibrate_Gyro(BMI270_Data_t *bias)
{
	if (bias == NULL) return HAL_ERROR;
	printf("[INFO] Calibrating Gyroscope... Keep the device still.\r\n");

    float sum_x = 0, sum_y = 0, sum_z = 0;
    int sample_count = 100;
    BMI270_Data_t raw_gyro;

    for (int i = 0; i < sample_count; i++) {
        if (BMI270_Read_Gyro(&raw_gyro) == HAL_OK) {
            sum_x += raw_gyro.x;
            sum_y += raw_gyro.y;
            sum_z += raw_gyro.z;
        } else {
            return HAL_ERROR;
        }
        HAL_Delay(10);	// ODR 속도(100Hz)에 맞춰 10ms씩 대기
    }

    // 결과값을 인자로 들어온 bias 구조체 주소에 저장
    bias->x = sum_x / (float)sample_count;
    bias->y = sum_y / (float)sample_count;
    bias->z = sum_z / (float)sample_count;

    printf("[SUCCESS] Calibration Done! Bias -> X:%.1f, Y:%.1f, Z:%.1f dps\r\n",
    		bias->x, bias->y, bias->z);

    return HAL_OK;
}

// 가속도계 XYZ축 데이터 일괄 복사 및 물리 단위(g) 변환
HAL_StatusTypeDef BMI270_Read_Accel(BMI270_Data_t *accel)
{
	if (accel == NULL) return HAL_ERROR;

	uint8_t tx_buf[8] = {BMI270_REG_ACC_DATA_X | 0x80, 0,};
	uint8_t rx_buf[8] = {0,};

	HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
	HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
	HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

	if (status != HAL_OK) return status;

	// 더미 바이트(rx_buf[1]) 이후 2, 3번째 인덱스부터 X축 시작
	int16_t raw_x = (int16_t)((rx_buf[3] << 8) | rx_buf[2]);
	int16_t raw_y = (int16_t)((rx_buf[5] << 8) | rx_buf[4]);
	int16_t raw_z = (int16_t)((rx_buf[7] << 8) | rx_buf[6]);

	// 풀스케일 ±2g 설정 기준: 16384 LSB = 1g
	accel->x = (float)raw_x / 16384.0f;
	accel->y = (float)raw_y / 16384.0f;
	accel->z = (float)raw_z / 16384.0f;

	return HAL_OK;
}

// 자이로스코프 XYZ축 데이터 일괄 복사 및 물리 단위(dps) 변환
HAL_StatusTypeDef BMI270_Read_Gyro(BMI270_Data_t *gyro)
{
    if (gyro == NULL) return HAL_ERROR;

    uint8_t tx_buf[8] = {BMI270_REG_GYR_X_LSB | 0x80, 0,};
    uint8_t rx_buf[8] = {0,};

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) return status;

    int16_t raw_x = (int16_t)((rx_buf[3] << 8) | rx_buf[2]);
    int16_t raw_y = (int16_t)((rx_buf[5] << 8) | rx_buf[4]);
    int16_t raw_z = (int16_t)((rx_buf[7] << 8) | rx_buf[6]);

    // 풀스케일 ±2000dps 설정 기준: 16.384 LSB = 1dps
    gyro->x = (float)raw_x / 16.384f;
    gyro->y = (float)raw_y / 16.384f;
    gyro->z = (float)raw_z / 16.384f;

    return HAL_OK;
}
