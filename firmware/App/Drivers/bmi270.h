#ifndef APP_DRIVERS_BMI270_H_
#define APP_DRIVERS_BMI270_H_

#include "main.h"	// HAL 라이브러리와 핀 이름들을 가져오기 위해 필수

// --- [레지스터 주소 매크로] ---
#define BMI270_REG_CHIP_ID    	 0x00	// 칩 ID 확인 레지스터
#define BMI270_REG_INTERNAL_STAT 0x21	// 내부 상태 확인 레지스터
#define BMI270_REG_INIT_CTRL  	 0x59  	// 초기화 제어 레지스터
#define BMI270_REG_PWR_CONF   	 0x7C  	// 전원 설정 레지스터
#define BMI270_REG_PWR_CTRL   	 0x7D  	// 전원 제어 레지스터

#define BMI270_REG_INIT_ADDR_0   0x5B  	// 스트림 주소 지정 레지스터 - 하위 4비트 (LSB)
#define BMI270_REG_INIT_ADDR_1   0x5C  	// 스트림 주소 지정 레지스터 - 상위 8비트 (MSB)
#define BMI270_REG_INIT_DATA     0x5E  	// 설정 데이터 스트림 입력 레지스터

#define BMI270_REG_ACC_DATA_X 	 0x0C  	// 가속도 X축 데이터 시작 주소
#define BMI270_REG_ACC_CONF      0x40	// 가속도 데이터 속도(ODR) 및 대역폭 설정 레지스터
#define BMI270_REG_ACC_RANGE     0x41   // 가속도 측정 범위 설정 레지스터

#define BMI270_REG_GYR_X_LSB     0x12  	// 자이로 X축 데이터 시작 주소
#define BMI270_REG_GYR_CONF      0x42  	// 자이로 ODR 및 대역폭 설정 레지스터
#define BMI270_REG_GYR_RANGE     0x43  	// 자이로 측정 범위 설정 레지스터

// --- [기대하는 설정 값] ---
#define BMI270_CHIP_ID_VAL    	 0x24

// --- [통합 데이터 구조체] ---
typedef struct {
    float x;
    float y;
    float z;
} BMI270_Data_t;

// --- [하위 레벨 통신 함수] ---
// HAL_StatusTypeDef: 통신 성공 여부를 반환
HAL_StatusTypeDef BMI270_ReadRegister(uint8_t reg, uint8_t *val);
HAL_StatusTypeDef BMI270_WriteRegister(uint8_t reg, uint8_t val);

// --- [상위 레벨 API 함수] ---
HAL_StatusTypeDef BMI270_Init(void);
HAL_StatusTypeDef BMI270_Calibrate_Gyro(BMI270_Data_t *bias);
HAL_StatusTypeDef BMI270_Read_Accel(BMI270_Data_t *accel);
HAL_StatusTypeDef BMI270_Read_Gyro(BMI270_Data_t *gyro);

#endif /* BMI270_H_ */
