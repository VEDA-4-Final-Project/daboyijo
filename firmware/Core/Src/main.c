/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Optimized Fall Detection & Health Monitoring System (50Hz)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bmi270.h"
#include "max30102.h"
#include "fall_detection.h"
#include "heart_rate_calc.h"
#include "usbd_cdc_if.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
BMI270_Data_t gyroBias = {0};

/* 실시간 비동기 제어용 상태 동기화 플래그 */
volatile uint8_t g_timer_fired = 0;
volatile uint8_t g_spi_ready = 0;
volatile uint8_t g_i2c_ready = 0;

/* 동적 잡음 억제(Motion Blanking)용 최신 자이로 데이터 글로벌 보관소 */
volatile float g_latest_gyro_x = 0.0f;
volatile float g_latest_gyro_y = 0.0f;
volatile float g_latest_gyro_z = 0.0f;

/* SPI 비동기 통신 전용 버퍼 */
static uint8_t spi_tx_buf[14] = {0};
static uint8_t spi_raw_buf[14] = {0};

/* I2C 비동기 통신 전용 버퍼 및 구조체 (MAX30102) */
static uint8_t i2c_raw_buf[6] = {0};
MAX30102_Data_t maxData = {0};

/* 알림 메시지 버퍼 */
uint8_t tx_alert_buf[] = "🚨 FALL DETECTED!\r\n";
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void SPI2_DMA_Reset_Unlock(void);
static void Process_IMU_Data(void);
static void Process_PPG_Data(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USB_DEVICE_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(2500); // 센서 전원 및 아날로그 회로 안정화 대기

  /* 1. 하드웨어 및 센서 초기화 */
  if (BMI270_Init() != HAL_OK) {
      printf(">> [ERROR] BMI270 Init Failed!\r\n");
      while(1) {
          HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
          HAL_Delay(100);
      }
  }

  if (MAX30102_Init() != HAL_OK) {
      printf(">> [ERROR] MAX30102 Init Failed!\r\n");
      while(1) {
          HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
          HAL_Delay(50);
      }
  }

  /* 2. 알고리즘 초기화 및 센서 캘리브레이션 */
  if (BMI270_Calibrate_Gyro(&gyroBias) != HAL_OK) {
      printf("[WARNING] Gyro Calibration Failed! Using default zero bias.\r\n");
  }

  FallDetection_Init(gyroBias);
  HeartRateCalc_Init();

  /* 3. 통신 라인 안전 초기화 */
  SPI2_DMA_Reset_Unlock();

  /* 4. 시스템 메인 타이머 (50Hz) 인터럽트 시작 */
  __HAL_TIM_CLEAR_IT(&htim3, TIM_IT_UPDATE);
  __HAL_TIM_SET_COUNTER(&htim3, 0);

  if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK) {
      printf(">> [ERROR] Failed to start Timer3 IT!\r\n");
  }
  printf(">> Realtime Monitoring Start\r\n\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      /* ------------------------------------------------------------------
       * [STAGE 1] TIM3 타이머 인터럽트 트리거 블록 (20ms 주기)
       * ------------------------------------------------------------------ */
	  if (g_timer_fired == 1)
	  {
		  g_timer_fired = 0;

		  if (hspi2.State == HAL_SPI_STATE_READY)
		  {
			  memset(spi_tx_buf, 0, sizeof(spi_tx_buf));
			  spi_tx_buf[0] = 0x0C | 0x80; // Register 0x0C read 명령

			  HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
			  HAL_StatusTypeDef spi_stat = HAL_SPI_TransmitReceive_DMA(&hspi2, spi_tx_buf, spi_raw_buf, 14);

			  if (spi_stat != HAL_OK)
			  {
				  HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
				  SPI2_DMA_Reset_Unlock();
			  }
		  }
		  else
		  {
			  SPI2_DMA_Reset_Unlock();
		  }
	  }

      /* ------------------------------------------------------------------
       * [STAGE 2 & 3] 데이터 처리 및 알고리즘 구동
       * ------------------------------------------------------------------ */
      Process_IMU_Data();
      Process_PPG_Data();
  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
static void Process_IMU_Data(void)
{
    if (g_spi_ready == 1)
    {
        g_spi_ready = 0;

        BMI270_Data_t accel_data = {0};
        BMI270_Data_t gyro_data = {0};

        BMI270_Parse_DMA_Data(spi_raw_buf, &accel_data, &gyro_data, &gyroBias);

        g_latest_gyro_x = gyro_data.x;
        g_latest_gyro_y = gyro_data.y;
        g_latest_gyro_z = gyro_data.z;

        uint8_t is_worn_flag = HeartRateCalc_IsWorn();

        FallDetection_Update(&accel_data, &gyro_data, is_worn_flag);
    }
}

static void Process_PPG_Data(void)
{
    if (g_i2c_ready == 1)
    {
        g_i2c_ready = 0;

        MAX30102_Parse_DMA_Data(i2c_raw_buf, &maxData);

        HeartRateCalc_Process_DMA(&maxData, g_latest_gyro_x, g_latest_gyro_y, g_latest_gyro_z);

        uint8_t dummy_stat = 0;
        MAX30102_ReadRegister(MAX30102_REG_INT_STAT_1, &dummy_stat);
    }
}

// 낙상 확정 시 UART 송신
void Send_Fall_Alert_Hardware(void)
{
    if (huart2.gState == HAL_UART_STATE_READY)
    {
        HAL_UART_Transmit_DMA(&huart2, tx_alert_buf, sizeof(tx_alert_buf) - 1);
    }
}

// SPI 데드락 탈출
void SPI2_DMA_Reset_Unlock(void)
{
    HAL_SPI_DMAStop(&hspi2);
    __HAL_SPI_DISABLE(&hspi2);
    __HAL_SPI_ENABLE(&hspi2);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
}

// 타이머 인터럽트
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        g_timer_fired = 1;
    }
}

// 심박센서 인터럽트
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
    {
    	if(g_i2c_ready == 0)
    	{
    		HAL_I2C_Mem_Read_DMA(&hi2c1, MAX30102_I2C_ADDR, MAX30102_REG_FIFO_DATA,
    				I2C_MEMADD_SIZE_8BIT, i2c_raw_buf, 6);
    	}
    }
}

// I2C 수신 완료
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        g_i2c_ready = 1;
    }
}

// SPI 수신 완료
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
        g_spi_ready = 1;
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_SPI_TxRxCpltCallback(hspi);
    }
}

int _write(int file, char *ptr, int len)
{
    extern USBD_HandleTypeDef hUsbDeviceFS;
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
    {
        return len;
    }

    volatile uint32_t retry_count = 2000;
    while (CDC_Transmit_FS((uint8_t*)ptr, len) == USBD_BUSY)
    {
        retry_count--;
        if (retry_count == 0)
        {
            break;
        }
    }
    return len;
}
/* USER CODE END 4 */

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
