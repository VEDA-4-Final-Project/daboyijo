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
#include "hm10.h"
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

/* HM-10 낙상 의심 플래그 (확정 시 세우고, 일정 시간 유지 후 자동 해제) */
volatile uint8_t  g_fall_flag = 0;
volatile uint32_t g_fall_flag_ms = 0;
#define HM10_FALL_HOLD_MS   5000   /* 낙상 플래그 유지 시간(ms) — 카메라 교차검증 윈도우 확보 */
#define HM10_VITAL_PERIOD_MS 1000  /* 바이탈 주기 송신 간격(ms) */

/* 센서 사용 가능 여부. 초기화에 실패해도 멈추지 않고 해당 센서만 끈 채로 계속 돈다.
 * 한쪽이 죽어도 나머지 기능과 HM-10 송신은 살아있어야 하기 때문. */
static uint8_t g_bmi270_ok = 0;
static uint8_t g_max30102_ok = 0;
#define SENSOR_INIT_RETRY   3      /* 초기화 재시도 횟수 */
#define SENSOR_RETRY_WAIT_MS 200   /* 재시도 간격(ms) */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void SPI2_DMA_Reset_Unlock(void);
static void Process_IMU_Data(void);
static void Process_PPG_Data(void);
static void HM10_Send_Now(void);
static void Blink_Error_Code(uint8_t count);
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
  hm10_init(&huart2); // HM-10을 USART2에 등록 (기존 낙상 알림 UART 재활용)

  HAL_Delay(2500); // 센서 전원 및 아날로그 회로 안정화 대기

  /* 1. 하드웨어 및 센서 초기화
   *    실패해도 멈추지 않는다. 재시도 후 해당 센서만 끄고 진행한다.
   *    여기서 갇히면 웨어러블이 조용히 죽어버리고 서버는 이유를 알 수 없다. */
  for (int i = 0; i < SENSOR_INIT_RETRY && !g_bmi270_ok; i++) {
      if (BMI270_Init() == HAL_OK) {
          g_bmi270_ok = 1;
      } else {
          printf(">> [WARNING] BMI270 Init Failed! (%d/%d)\r\n", i + 1, SENSOR_INIT_RETRY);
          HAL_Delay(SENSOR_RETRY_WAIT_MS);
      }
  }

  for (int i = 0; i < SENSOR_INIT_RETRY && !g_max30102_ok; i++) {
      if (MAX30102_Init() == HAL_OK) {
          g_max30102_ok = 1;
      } else {
          printf(">> [WARNING] MAX30102 Init Failed! (%d/%d)\r\n", i + 1, SENSOR_INIT_RETRY);
          HAL_Delay(SENSOR_RETRY_WAIT_MS);
      }
  }

  /* 최종 실패한 센서는 LED 깜빡임 횟수로 알린다 (USB 미연결 시 유일한 단서) */
  if (!g_bmi270_ok) {
      printf(">> [ERROR] BMI270 disabled — 낙상 감지 사용 불가\r\n");
      Blink_Error_Code(2);
  }
  if (!g_max30102_ok) {
      printf(">> [ERROR] MAX30102 disabled — 심박/SpO2 사용 불가\r\n");
      Blink_Error_Code(3);
  }

  /* 2. 알고리즘 초기화 및 센서 캘리브레이션 */
  if (g_bmi270_ok && BMI270_Calibrate_Gyro(&gyroBias) != HAL_OK) {
      printf("[WARNING] Gyro Calibration Failed! Using default zero bias.\r\n");
  }

  FallDetection_Init(gyroBias);   /* BMI270 실패 시 gyroBias 는 0 초기값 그대로 */
  HeartRateCalc_Init();

  /* 3. 통신 라인 안전 초기화 */
  SPI2_DMA_Reset_Unlock();

  /* 4. 시스템 메인 타이머 (50Hz) 인터럽트 시작 */
  __HAL_TIM_CLEAR_IT(&htim3, TIM_IT_UPDATE);
  __HAL_TIM_SET_COUNTER(&htim3, 0);

  if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK) {
      printf(">> [ERROR] Failed to start Timer3 IT!\r\n");
  }
  printf(">> Realtime Monitoring Start (IMU:%s PPG:%s)\r\n\r\n",
         g_bmi270_ok ? "ok" : "OFF", g_max30102_ok ? "ok" : "OFF");
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

		  if (!g_bmi270_ok)
		  {
			  /* IMU 비활성 — SPI 폴링 건너뛴다 */
		  }
		  else if (hspi2.State == HAL_SPI_STATE_READY)
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

      /* ------------------------------------------------------------------
       * [STAGE 4] HM-10 BLE 주기 송신 (1Hz) — 바이탈 + 낙상 플래그
       * ------------------------------------------------------------------ */
      static uint32_t last_vital_ms = 0;
      uint32_t now = HAL_GetTick();

      /* 낙상 플래그 유지 시간 경과 시 자동 해제 */
      if (g_fall_flag && (now - g_fall_flag_ms >= HM10_FALL_HOLD_MS))
      {
          g_fall_flag = 0;
      }

      if (now - last_vital_ms >= HM10_VITAL_PERIOD_MS)
      {
          last_vital_ms = now;
          HM10_Send_Now();
      }
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

// 현재 바이탈 + 낙상 플래그를 HM-10 5바이트 패킷으로 송신
static void HM10_Send_Now(void)
{
    uint32_t bpm  = HeartRateCalc_GetBPM();
    uint32_t spo2 = HeartRateCalc_GetSpO2();
    uint8_t  hr   = (bpm > 255) ? 255 : (uint8_t)bpm;   // 심박 8bit 클램프

    // 온도: temperature_calc 미구현 → 우선 0 더미
    uint8_t temp = 0;

    hm10_send_packet(hr, (uint8_t)spo2, temp, g_fall_flag);
}

// 낙상 확정 시 호출 (fall_detection.c) → 플래그 세우고 즉시 1회 송신
void Send_Fall_Alert_Hardware(void)
{
    g_fall_flag = HM10_FALL_SUSPECT;
    g_fall_flag_ms = HAL_GetTick();
    HM10_Send_Now();   // 다음 주기(1s) 기다리지 않고 즉시 반영
}

// 부팅 시 센서 실패를 LED 깜빡임 횟수로 알린다 (USB 미연결 시 유일한 단서)
//   2회 = BMI270 실패, 3회 = MAX30102 실패
// 부팅 때 한 번만 도는 유한 루프다. 여기서 갇히면 안 된다.
static void Blink_Error_Code(uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
        HAL_Delay(200);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
        HAL_Delay(200);
    }
    HAL_Delay(600);   // 다음 코드와 구분되게 쉬어간다
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
    	if(g_i2c_ready == 0 && g_max30102_ok)
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
