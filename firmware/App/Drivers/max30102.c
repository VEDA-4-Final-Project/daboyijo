#include "max30102.h"
#include <stdio.h>

extern I2C_HandleTypeDef hi2c1;

HAL_StatusTypeDef MAX30102_ReadRegister(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return HAL_ERROR;
    return HAL_I2C_Mem_Read(&hi2c1, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, val, 1, 100);
}

HAL_StatusTypeDef MAX30102_WriteRegister(uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(&hi2c1, MAX30102_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

HAL_StatusTypeDef MAX30102_Init(void)
{
    uint8_t part_id = 0;
    printf("=== MAX30102 Clean Initialization ===\r\n");

    if (MAX30102_ReadRegister(MAX30102_REG_PART_ID, &part_id) != HAL_OK) return HAL_ERROR;
    if (part_id != MAX30102_PART_ID_VAL) {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", part_id);
        return HAL_ERROR;
    }

    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x40) != HAL_OK) return HAL_ERROR;
    HAL_Delay(50);

    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_1, 0xC0) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_INT_ENABLE_2, 0x00) != HAL_OK) return HAL_ERROR;

    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_WR_PTR, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_OVF_COUNTER, 0x00) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_RD_PTR, 0x00) != HAL_OK) return HAL_ERROR;

    if (MAX30102_WriteRegister(MAX30102_REG_FIFO_CONF, 0x5F) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_MODE_CONF, 0x03) != HAL_OK) return HAL_ERROR;

    if (MAX30102_WriteRegister(MAX30102_REG_SPO2_CONF, 0x27) != HAL_OK) return HAL_ERROR;

    if (MAX30102_WriteRegister(MAX30102_REG_LED1_PA, 0x24) != HAL_OK) return HAL_ERROR;
    if (MAX30102_WriteRegister(MAX30102_REG_LED2_PA, 0x24) != HAL_OK) return HAL_ERROR;

    uint8_t dummy = 0;
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_1, &dummy);
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_2, &dummy);

    printf("[ SUCCESS ] MAX30102 Ready!\r\n");
    return HAL_OK;
}

void MAX30102_Parse_DMA_Data(uint8_t *dma_buf, MAX30102_Data_t *sensor_data)
{
    if (dma_buf == NULL || sensor_data == NULL) return;

    uint32_t raw_red = ((uint32_t)dma_buf[0] << 16) | ((uint32_t)dma_buf[1] << 8) | dma_buf[2];
    uint32_t raw_ir  = ((uint32_t)dma_buf[3] << 16) | ((uint32_t)dma_buf[4] << 8) | dma_buf[5];

    sensor_data->red = raw_red & 0x0003FFFF;
    sensor_data->ir  = raw_ir  & 0x0003FFFF;
}
