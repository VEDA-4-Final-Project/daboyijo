#include "bmi270.h"
#include "bmi270_config.h"
#include <stdio.h>

extern SPI_HandleTypeDef hspi2;

// 1바이트 읽기. 주소(1B) + 더미(1B)+ 데이터(1B)
uint8_t BMI270_ReadRegister(uint8_t reg)
{
    uint8_t tx_buf[3] = {reg | 0x80, 0x00, 0x00};
    uint8_t rx_buf[3] = {0,};

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    return rx_buf[2];
}
// 1바이트 쓰기
void BMI270_WriteRegister(uint8_t reg, uint8_t val)
{
    uint8_t tx_buf[2] = {reg & 0x7F, val};

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi2, tx_buf, 2, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
}

// 가속도 XYZ축 한번에 읽기 함수. 주소(1B) + 더미(1B) + 데이터(6B)
void BMI270_Read_Accel(int16_t *x, int16_t *y, int16_t *z)
{
	uint8_t tx_buf[8] = {BMI270_REG_ACC_DATA_X | 0x80, 0,};
	uint8_t rx_buf[8] = {0,};

	HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
	HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
	HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

	*x = (int16_t)((rx_buf[3] << 8) | rx_buf[2]);
	*y = (int16_t)((rx_buf[5] << 8) | rx_buf[4]);
	*z = (int16_t)((rx_buf[7] << 8) | rx_buf[6]);
}

// 센서 초기화 함수
void BMI270_Init(void)
{
    printf("\r\n=== BMI270 Clean Initialization ===\r\n");

    // 1. 칩 ID 확인
    if (BMI270_ReadRegister(BMI270_REG_CHIP_ID) != BMI270_CHIP_ID_VAL) {
        printf("[ FAIL ] Sensor Not Found!\r\n");
        return;
    }

    // 2. 준비 운동 (절전 모드 해제 및 업로드 준비)
    BMI270_WriteRegister(BMI270_REG_PWR_CONF, 0x00);
    HAL_Delay(2);
    BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x00);
    HAL_Delay(2);

    // 3. 256바이트씩 순수한 Chunk 업로드
    for (uint16_t i = 0; i < 8192; i += 256)
    {
    	// BMI270 메모리는 2바이트 단위
		uint16_t word_addr = i / 2;

		// 주소 지정 레지스터에 주소 넣기 (4096 = 12비트. 하위 4비트 + 상위 8비트)
		BMI270_WriteRegister(BMI270_REG_INIT_ADDR_0, word_addr & 0x0F);
		BMI270_WriteRegister(BMI270_REG_INIT_ADDR_1, (word_addr >> 4) & 0xFF);

		// CS 내리고 주소(1B) 쏜 후, 곧바로 데이터(256B) 연속 스트리밍
		uint8_t reg_init_data = BMI270_REG_INIT_DATA;
		HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
		HAL_SPI_Transmit(&hspi2, &reg_init_data, 1, 10);
		HAL_SPI_Transmit(&hspi2, (uint8_t*)&bmi270_config_file[i], 256, 100);
		HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

		HAL_Delay(1);
    }

    // 4. 초기화 완료 선언 및 안정화 대기
    BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x01);
	HAL_Delay(30);

    // 5. 상태 검증 및 가속도계 활성화
    if (BMI270_ReadRegister(BMI270_REG_INTERNAL_STAT) == 0x01) {
        printf("[ SUCCESS ] BMI270 Ready!\r\n");

        BMI270_WriteRegister(BMI270_REG_PWR_CTRL, 0x04); // Accel ON
        HAL_Delay(10);
        BMI270_WriteRegister(BMI270_REG_ACC_CONF, 0xA8); // 가속도계 ODR, BW 설정
        HAL_Delay(10);
        BMI270_WriteRegister(BMI270_REG_ACC_RANGE, 0x00);// 가속도계 ±2g 범위 설정
        HAL_Delay(50);
    }
    else {
        printf("[ FAIL ] Init Failed!\r\n");
    }
}
