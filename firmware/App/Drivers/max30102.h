#ifndef APP_DRIVERS_MAX30102_H_
#define APP_DRIVERS_MAX30102_H_

#include "main.h"
#include <stdint.h>

/* --- [ MAX30102 레지스터 주소 맵 ] --- */
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

/* --- [ 데이터 구조체 ] --- */
typedef struct {
    uint32_t red;
    uint32_t ir;
} MAX30102_Data_t;

/* --- [ 하위 레벨 통신 API ] --- */
HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val);
HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val);

/* --- [ 상위 인터페이스 API ] --- */
HAL_StatusTypeDef MAX30102_Init(void);
void MAX30102_Parse_DMA_Data(uint8_t *dma_buf, MAX30102_Data_t *sensor_data);

#endif /* APP_DRIVERS_MAX30102_H_ */
