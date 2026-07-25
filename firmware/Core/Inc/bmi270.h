#ifndef INC_BMI270_H_
#define INC_BMI270_H_

#include "main.h"	// HAL 라이브러리와 핀 이름들을 가져오기 위해 필수

// --- [레지스터 주소 매크로] ---
#define BMI270_REG_CHIP_ID    	 0x00	// 칩 ID 확인 레지스터
#define BMI270_REG_PWR_CONF   	 0x7C  	// 전원 설정 레지스터
#define BMI270_REG_INIT_CTRL  	 0x59  	// 초기화 제어 레지스터
#define BMI270_REG_PWR_CTRL   	 0x7D  	// 전원 제어 레지스터
#define BMI270_REG_INTERNAL_STAT 0x21	// 내부 상태 확인 레지스터
#define BMI270_REG_ACC_DATA_X 	 0x0C  	// 가속도 X축 데이터 시작점 (X, Y, Z가 연속됨)
#define BMI270_REG_ACC_CONF      0x40	// 가속도계 데이터 속도(ODR) 및 대역폭 설정 레지스터
#define BMI270_REG_ACC_RANGE     0x41   // 가속도 범위 설정 레지스터
#define BMI270_REG_INIT_ADDR_0   0x5B  	// 스트림 주소 지정 레지스터 - 하위 4비트 (LSB)
#define BMI270_REG_INIT_ADDR_1   0x5C  	// 스트림 주소 지정 레지스터 - 상위 8비트 (MSB)
#define BMI270_REG_INIT_DATA     0x5E  	// 설정 데이터 스트림 입력 레지스터

// --- [기대하는 설정 값] ---
#define BMI270_CHIP_ID_VAL    0x24

// --- [외부에서 사용할 함수 선언] ---
void BMI270_Init(void);
uint8_t BMI270_ReadRegister(uint8_t reg);
void BMI270_WriteRegister(uint8_t reg, uint8_t val);
void BMI270_Read_Accel(int16_t *x, int16_t *y, int16_t *z);

#endif /* BMI270_H_ */
