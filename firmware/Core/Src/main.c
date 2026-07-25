/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "bmi270.h"	//bmi270 드라이버
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
BMI270_Data_t accData;
BMI270_Data_t gyrData;
BMI270_Data_t gyroBias = {0};	// 자이로 영점 저장
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

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
  MX_I2C1_Init();
  MX_SPI2_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(300);
  if (BMI270_Init() != HAL_OK) {
      printf("[ERROR] BMI270 Hardware Link Fault!\r\n");
      // 하드웨어 에러 시 무한 루프에 가두거나 에러 플래그 처리
      while(1) {
          HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13); // 에러 알림 LED 깜빡임
          HAL_Delay(100);
      }
  }

  // === 자이로 영점 잡기 (약 1초간 100개 샘플링) ===
  printf("[INFO] Calibrating Gyroscope... Keep the device still.\r\n");
  float sum_x = 0, sum_y = 0, sum_z = 0;
  int sample_count = 100;
  BMI270_Data_t raw_gyro;

  for (int i = 0; i < sample_count; i++) {
	  if (BMI270_Read_Gyro(&raw_gyro) == HAL_OK) {
		  sum_x += raw_gyro.x;
		  sum_y += raw_gyro.y;
		  sum_z += raw_gyro.z;
	  }
	  HAL_Delay(10); // ODR 속도(100Hz)에 맞춰 10ms씩 대기
  }

  // 평균 오프셋 계산 후 저장
  gyroBias.x = sum_x / (float)sample_count;
  gyroBias.y = sum_y / (float)sample_count;
  gyroBias.z = sum_z / (float)sample_count;

  printf("[SUCCESS] Calibration Done! Bias -> X:%.1f, Y:%.1f, Z:%.1f dps\r\n",
		  gyroBias.x, gyroBias.y, gyroBias.z);
  // ====================================================

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  if (BMI270_Read_Accel(&accData) == HAL_OK && BMI270_Read_Gyro(&gyrData) == HAL_OK)
	  {
		  gyrData.x -= gyroBias.x;
		  gyrData.y -= gyroBias.y;
		  gyrData.z -= gyroBias.z;

		  printf("[ACC] X:%.3f Y:%.3f Z:%.3f g | [GYR] X:%.1f Y:%.1f Z:%.1f dps\r\n",
				 accData.x, accData.y, accData.z, gyrData.x, gyrData.y, gyrData.z);
	  }
	  else
	  {
		  printf("[ WARNING ] Sensor Data Disconnected during runtime!\r\n");
	  }

	  HAL_Delay(300);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
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

  /** Initializes the CPU, AHB and APB buses clocks
  */
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
int _write(int file, char *ptr, int len)
{
  // 1. 문장(ptr)을 길이(len)만큼 한 번에 USB로 쏜다.
  CDC_Transmit_FS((uint8_t*)ptr, len);

  // 2. USB가 택배를 다 보낼 때까지 아주 잠깐(1ms) 기다려준다. (데이터 씹힘 방지)
  HAL_Delay(1);

  return len;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
