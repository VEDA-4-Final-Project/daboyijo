/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.c
  * @brief   센서 인터럽트 + FIFO 블록 기반 웨어러블 (낙상 / 심박 / SpO2)
  ******************************************************************************
  *
  * ── 아키텍처 ────────────────────────────────────────────────────────
  *
  *   센서가 준비되면 인터럽트로 알리고, MCU 는 그때만 깨어나 블록을 수거한다.
  *
  *     MAX30102  FIFO 25샘플(0.5초) → EXTI PB0 → I2C DMA 150B
  *     BMI270    FIFO 워터마크      → EXTI PA5 → SPI DMA (가속도 전용)
  *     그 외 시간에는 __WFI() 로 취침
  *
  *   알고리즘이 샘플 하나가 아니라 배열을 받으므로 앞뒤 문맥을 볼 수 있다.
  *   낙상 백트래킹과 PPG 영위상 필터가 이 구조를 전제로 한다.
  *
  * ── 메인 루프 (순서에 의미가 있다) ──────────────────────────────────
  *
  *   IWDG 갱신           루프가 살아있다는 증거
  *   Service_I2C_Fault() I2C 오류 복구 — ISR 이 아니라 여기서 한다
  *   Collect_IMU_Block() FIFO 길이 조회 후 SPI DMA 시작
  *   Process_IMU_Block() 낙상 판정 → 움직임 값 산출
  *   Collect_PPG_Block() EXTI 유실 / BUSY 고착 시 스트림 복구
  *   Process_PPG_Block() AGC → 심박/SpO2 (IMU 움직임 값을 참조하므로 뒤에)
  *   HM10_Send_Now()     BLE 주기 송신 (1초)
  *   Enter_Idle()        할 일 없으면 취침
  *
  * ── 엣지 인터럽트 폴백 ──────────────────────────────────────────────
  *
  *   엣지 트리거는 한 번 놓치면 스스로 복구하지 못한다. 그래서 각 스트림에
  *   시간 기반 폴백을 둔다. 정상 동작 시 비용은 0 이고, 인터럽트가 죽어도
  *   시스템은 계속 돈다.
  *
  *     BMI270 INT1 미배선/유실   → 700ms 폴링
  *     MAX30102 EXTI 유실        → 1.5초 폴링
  *     I2C 핸들 BUSY 고착        → 3초 후 강제 복구
  *     그 위에 IWDG 8초          → 어느 단계든 멎으면 재부팅
  *
  * ── ISR 안에서 하면 안 되는 것 ──────────────────────────────────────
  *
  *   · HAL_Delay()  — SysTick(우선순위 15)이 선점하지 못해 영원히 멈춘다.
  *                    복구가 필요하면 플래그만 세우고 메인 루프에 맡긴다.
  *   · 같은 핸들에 대한 블로킹/DMA 혼용 — 상태 머신이 깨진다.
  *     (g_ppg_block_ready 플래그가 hi2c1 의 잠금 역할을 겸한다)
  *
  * ── I2C 버스 락업 ───────────────────────────────────────────────────
  *
  *   DMA 전송 도중 MCU 가 리셋되면 슬레이브가 SDA 를 계속 붙든 채 남는다.
  *   MCU 재시작만으로는 풀리지 않아 첫 부팅 이후 통신이 되지 않는다.
  *   MX_I2C1_Init() 보다 먼저 I2C1_BusRecovery() 로 SCL 을 직접 흔들어
  *   슬레이브를 빼낸다.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
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
#include "step_counter.h"
#include "wrist_raise.h"
#include "app_clock.h"
#include "usbd_cdc_if.h"
#include "hm10.h"
#include "display_service.h"
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

/* ------------------------------------------------------------------
 * [저전력 모드 선택]
 *   0 = WFI Sleep. USB CDC 로그가 살아있어 알고리즘 튜닝이 가능하다.
 *   1 = Stop Mode. USB 가 끊기므로 로그를 잃는다. 튜닝 완료 후 전환할 것.
 *
 * Stop Mode 진입 시 HSE/PLL 이 정지하므로 깨어난 직후 반드시 클럭을 복구해야
 * 하며, 그 사이 SysTick 이 멈춰 HAL_GetTick() 기반 주기가 어긋난다.
 * ------------------------------------------------------------------ */
#define LOWPOWER_STOP_MODE   0

/* 하드웨어 인터럽트 → 메인 루프 상태 동기화 플래그 */
volatile uint8_t g_imu_fwm_pending = 0;   // FIFO 워터마크 도달, 아직 수거 전
volatile uint8_t g_imu_block_ready = 0;   // SPI DMA 수거 완료
volatile uint8_t g_ppg_block_ready = 0;   // I2C DMA 수거 완료
volatile uint8_t g_i2c_fault       = 0;   // I2C 오류 — 복구는 메인 루프가 담당

/* IMU 인터럽트 배선 여부 판별 및 폴링 폴백 */
volatile uint8_t g_imu_int_seen    = 0;   // INT1 이 한 번이라도 들어왔는가
static uint8_t   g_imu_fallback_warned = 0;
static uint32_t  g_last_imu_block_ms = 0;
#define IMU_FALLBACK_MS   700             // 이 시간 동안 블록이 없으면 직접 수거

/* PPG 폴백.
 *
 * MAX30102 의 INT 는 액티브 로우이고 EXTI 는 하강 엣지 트리거다.
 * FIFO 가 찬 상태(INT=LOW)에서 I2C 복구나 재초기화가 일어나면 핀이 눌린 채
 * 남아 새 하강 엣지가 오지 않고, EXTI 에만 의존하는 PPG 스트림은 영구 정지한다.
 * 일정 시간 블록이 없으면 직접 수거해 FIFO 를 비우고 핀을 풀어준다. */
static uint32_t  g_last_ppg_block_ms = 0;
static uint32_t  g_ppg_fallback_count = 0;   // 엣지 유실로 폴링 복구한 횟수
static uint32_t  g_i2c_recover_count  = 0;   // I2C 오류로 버스 복구한 횟수
#define PPG_FALLBACK_MS   1500            // 정상 주기 0.5초의 3배

/* I2C BUSY 고착 워치독.
 *
 * DMA 가 오류 콜백 없이 멈추면 hi2c1.State 가 BUSY_RX 에 남는다. EXTI 경로와
 * 폴백 경로가 둘 다 State == READY 를 요구하고, g_i2c_fault 는 이미 소비되어
 * 0 이므로, 상태를 되돌려줄 주체가 아무도 없는 교착이 된다.
 * BUSY 가 비정상적으로 오래 지속되면 시간으로 그 교착을 연다. */
static uint32_t  g_i2c_busy_since_ms = 0;
#define I2C_BUSY_STUCK_MS  3000           // 정상 트랜잭션은 수 ms 안에 끝난다

/* PPG 센서 재초기화 (충격으로 센서만 리셋된 경우의 복구).
 * 쿨다운은 재초기화 폭주를 막는다 —— 센서가 물리적으로 빠졌다면
 * 매 오류마다 50ms 짜리 초기화를 시도하다 메인 루프가 잠식된다. */
static uint32_t  g_last_ppg_reinit_ms = 0;
static uint32_t  g_ppg_reinit_count   = 0;
#define PPG_REINIT_COOLDOWN_MS  2000

/* BMI270 FIFO 블록 — SPI DMA 수신 원본 및 디코딩 결과 */
static uint8_t       bmi_fifo_rx[BMI270_FIFO_RX_LEN] = {0};
static BMI270_Data_t bmi_accel[BMI270_FIFO_MAX_FRAMES] = {0};
static volatile uint16_t g_imu_block_bytes = 0;

/* MAX30102 FIFO 블록 — I2C DMA 수신 원본 및 디코딩 결과 */
static uint8_t         ppg_rx[MAX30102_MAX_BLOCK_BYTES] = {0};
static MAX30102_Data_t ppg_samples[MAX30102_FIFO_DEPTH] = {0};

/* HM-10 낙상 의심 플래그 - 확정 시 세우고 일정 시간 뒤 자동 해제 */
volatile uint8_t  g_fall_flag = 0;
volatile uint32_t g_fall_flag_ms = 0;
#define HM10_FALL_HOLD_MS   5000   /* 낙상 플래그 유지 시간(ms) — 카메라 교차검증 윈도우 확보 */

#define HM10_VITAL_PERIOD_MS 1000  /* 바이탈 주기 송신 간격 (ms) */

/* 주기 송신 시각. 낙상 즉시 송신도 이 시각을 갱신해야 패킷이 겹치지 않는다. */
static uint32_t g_last_vital_ms = 0;

/* 시각 요청 재시도.
 *
 * 부팅 직후에는 BLE 가 아직 안 붙어 있을 수 있어 요청 한 번은 유실되기 쉽다.
 * 동기가 설 때까지만 주기적으로 다시 묻고, 맞춰지면 스스로 멈춘다.
 * 3바이트짜리라 몇 번 더 보내도 비용이 없다. */
static uint32_t g_last_time_req_ms = 0;
#define TIME_REQ_RETRY_MS   5000

/* ── 독립 워치독(IWDG) ──────────────────────────────────────────────
 *
 * 어느 단계가 멎어도 BLE 송신만은 계속되는 상태가 가장 위험하다. 밖에서는
 * '측정값이 없는 정상 동작'과 구분되지 않기 때문이다. 사람이 차고 있는
 * 기기이므로 멈춘 채 살아있는 것보다 재부팅이 낫다.
 *
 * HAL_IWDG 모듈이 비활성이고 stm32f4xx_hal_iwdg.c 도 없어 레지스터로 직접
 * 다룬다 (KR/PR/RLR/SR 네 개가 전부다).
 *
 * LSI(약 32kHz) 기준 1000 × 256 / 32000 ≈ 8초. LSI 는 17~47kHz 로 흔들리므로
 * 최악의 경우에도 5.4초는 확보된다. 메인 루프의 최장 블로킹이 I2C 버스
 * 복구(약 22ms)라 여유가 충분하다. */
#define IWDG_KEY_RELOAD   0xAAAAU
#define IWDG_KEY_ENABLE   0xCCCCU
#define IWDG_KEY_UNLOCK   0x5555U
#define IWDG_RELOAD_VAL   1000U

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
static void Collect_IMU_Block(void);
static void Process_IMU_Block(void);
static void Process_PPG_Block(void);
static void HM10_Send_Now(void);
static void Blink_Error_Code(uint8_t count);
static void Enter_Idle(void);
static void I2C1_BusRecovery(void);
static void Service_I2C_Fault(void);
static void Collect_PPG_Block(void);
static void IWDG_Start(void);
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
  /* I2C 주변장치를 세우기 전에 버스부터 풀어준다.
   * MCU 가 DMA 전송 도중 리셋되면 슬레이브는 전송이 끝나지 않았다고 믿고
   * SDA 를 계속 Low 로 붙든다. MCU 재시작만으로는 절대 안 풀리고 센서 전원을
   * 뽑아야 복구되는데, 그게 "전원 인가 후 첫 부팅만 성공"의 정체다. */
  I2C1_BusRecovery();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USB_DEVICE_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  hm10_init(&huart2); /* HM-10 을 USART2 에 등록 (기존 낙상 알림 UART 재활용) */

  /* 수신 개시 — RPi 가 BLE 로 내려주는 시각을 받는다.
   * 이후에는 ISR 이 스스로 재무장하므로 여기서 한 번만 부르면 된다.
   * 실패는 조용히 넘기지 않는다 — 수신이 통째로 죽는데 겉으로는
   * '시계가 안 맞는다' 로만 보여 원인을 찾기 어렵다. */
  if (hm10_start_receive() != HAL_OK)
  {
      printf("[ HM10 ] 시각 수신 무장 실패 — 시각 동기 사용 불가\r\n");
  }

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

  /* 2. 알고리즘 초기화
   *
   * 자이로 캘리브레이션은 하지 않는다. 자이로 전원을 끈 상태이고
   * (bmi270.c 의 PWR_CTRL 주석 참조) 자이로 값을 읽는 코드도 없다.
   * 부팅 시간 2초를 그대로 절약한다. */
  FallDetection_Init();
  HeartRateCalc_Init();
  StepCounter_Init();
  WristRaise_Init();
  AppClock_Init();

  /* 3. 통신 라인 안전 초기화 */
  SPI2_DMA_Reset_Unlock();

  /* 4. FIFO 및 하드웨어 인터럽트 구성 (타이머 폴링 폐기) */
  if (g_bmi270_ok)
  {
      if (BMI270_ConfigFifoAndInterrupts() != HAL_OK) {
          printf(">> [ERROR] BMI270 FIFO/INT 구성 실패 — 낙상 감지 사용 불가\r\n");
          g_bmi270_ok = 0;
          Blink_Error_Code(4);
      }
      else {
          /* any-motion 은 "완전 정지 시 웨이크업 생략"용 절전 게이트일 뿐이다.
           * 실패해도 FIFO 워터마크만으로 감지는 정상 동작하므로 치명적이지 않다. */
          if (BMI270_ConfigAnyMotion(300, 100) != HAL_OK) {
              printf("[WARNING] any-motion 게이트 비활성 — 절전 효율만 저하됩니다.\r\n");
          }
      }
  }

  /* 폴백 타이머를 '지금' 으로 맞춘다.
   *
   * 0 으로 두면 부팅 시점의 HAL_GetTick() (센서 안정화 2.5초 + 초기화로 이미
   * 3초 남짓)이 곧바로 폴백 조건을 넘겨, 첫 인터럽트가 오기도 전에
   * '인터럽트 유실' 로 오판한다. */
  g_last_imu_block_ms = HAL_GetTick();
  g_last_ppg_block_ms = HAL_GetTick();

  printf(">> Block Monitoring Start (IMU:%s PPG:%s, idle:%s)\r\n\r\n",
         g_bmi270_ok ? "ok" : "OFF", g_max30102_ok ? "ok" : "OFF",
         LOWPOWER_STOP_MODE ? "STOP" : "WFI");

  /* 디스플레이는 꺼진 채로 세운다. 손목을 들 때까지 켜지지 않는다.
   * IWDG 보다 앞에 두는 이유는 내부 HAL_Delay 합계가 약 0.2초이기 때문이다. */
  DisplayService_Init(g_bmi270_ok);

  /* 부팅 시퀀스(센서 재시도 / Blink_Error_Code 의 HAL_Delay)가 모두 끝난
   * 뒤에 켠다. 이 위치가 중요하다 — 앞에서 켜면 재부팅 루프가 된다. */
  IWDG_Start();

  /* 첫 송신을 한 주기 뒤로 맞춘다. 0 으로 두면 부팅에 이미 3초 남짓 쓴 상태라
   * 기준점이 어긋난 채 시작한다. */
  g_last_vital_ms = HAL_GetTick();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      /* ------------------------------------------------------------------
       * [STAGE 1] FIFO 워터마크 도달 → SPI DMA 로 IMU 블록 일괄 수거
       * ------------------------------------------------------------------ */
      /* 워치독 갱신 —— 루프가 한 바퀴 돈다는 증거.
       * 어느 단계든 영구히 막히면 갱신이 끊기고 약 8초 뒤 리셋된다.
       * 조용히 멈춘 채로 BLE 에 0 만 내보내는 상태를 만들지 않기 위한 것이다. */
      IWDG->KR = IWDG_KEY_RELOAD;

      /* I2C 오류 복구는 반드시 여기서 한다 (ISR 안에서는 HAL_Delay 를 쓸 수 없다) */
      Service_I2C_Fault();

      Collect_IMU_Block();

      /* ------------------------------------------------------------------
       * [STAGE 2] 블록 단위 알고리즘 구동
       *   IMU 를 먼저 처리해야 PPG 모션 블랭킹이 최신 움직임 값을 본다.
       * ------------------------------------------------------------------ */
      Process_IMU_Block();

      Collect_PPG_Block();   /* EXTI 유실 시 스트림 복구 */
      Process_PPG_Block();

      /* ------------------------------------------------------------------
       * [STAGE 3] HM-10 BLE 주기 송신 (HM10_VITAL_PERIOD_MS = 1초) — 바이탈 + 낙상 플래그 + 걸음 수
       * ------------------------------------------------------------------ */
      /* 벽시계 진행 + 자정 처리.
       * 걸음 수는 Reset() 이 아니라 SetSteps(0) 으로 지운다 — 자정에 걷고 있는
       * 중일 수 있고, 그때 필터와 리듬 상태까지 날리면 진행 중이던 보행이
       * 끊겨 다음 4걸음을 다시 검증해야 한다. 지울 것은 누적값뿐이다. */
      /* RPi 가 내려준 시각이 있으면 반영한다.
       * ISR 은 검증만 하고 값을 남겨두며, 시계를 실제로 미는 것은 여기다.
       * AppClock_Service() 보다 먼저 부른다 — 시각을 갈아끼운 직후에 진행분을
       * 더하면 방금 맞춘 값이 한 틱 어긋난 채로 시작한다. */
      uint8_t sync_h, sync_m, sync_s;
      if (hm10_take_time(&sync_h, &sync_m, &sync_s))
      {
          AppClock_SetTime(sync_h, sync_m, sync_s);
          printf("[ CLOCK ] BLE 시각 동기 %02u:%02u:%02u\r\n",
                 (unsigned)sync_h, (unsigned)sync_m, (unsigned)sync_s);
      }

      AppClock_Service();

      if (AppClock_ConsumeMidnight())
      {
          printf("[ CLOCK ] 자정 — 걸음 수 초기화 (%lu 걸음)\r\n",
                 (unsigned long)StepCounter_GetSteps());
          StepCounter_SetSteps(0);
      }

      uint32_t now = HAL_GetTick();

      /* 시각이 아직 안 맞았으면 릴레이에 요청한다.
       * 릴레이의 10분 주기만 기다리면 그동안 화면에 빌드 시각이 남는데,
       * MCU 만 리셋되고 BLE 링크는 살아있는 경우 릴레이는 아무 일도 없었다고
       * 보기 때문에 특히 오래 걸린다. 먼저 물어보는 쪽이 확실하다. */
      if (!AppClock_IsSynced() && (now - g_last_time_req_ms >= TIME_REQ_RETRY_MS))
      {
          g_last_time_req_ms = now;
          hm10_request_time();

          /* 요청과 함께 수신 계측을 같이 찍는다.
           *
           * 시각이 안 맞는 현상은 원인이 셋인데 밖에서는 똑같아 보인다.
           *   RX=0            HM-10 TX → PA3 구간이 죽음. 릴레이가 뭘 보냈든 무관하다
           *   RX 증가 + bad 증가   바이트는 오는데 규격이 안 맞음 (양쪽 스펙 확인)
           *   RX 증가 + bad 0      0x55 가 아닌 다른 것이 옴 (HM-10 AT 응답 등)
           * err 가 함께 늘면 보드레이트나 노이즈를 의심한다. */
          uint32_t rx_total = 0, rx_err = 0, rx_bad = 0;
          uint8_t  rx_last = 0;
          hm10_get_rx_stats(&rx_total, &rx_err, &rx_bad, &rx_last);
          printf("[ HM10 ] 시각 요청 송신 — RX 누적 %lu B / 오류 %lu / 불량패킷 %lu / 마지막 0x%02X\r\n",
                 (unsigned long)rx_total, (unsigned long)rx_err,
                 (unsigned long)rx_bad, (unsigned)rx_last);
      }

      /* 낙상 플래그 유지 시간 경과 시 자동 해제 */
      if (g_fall_flag && (now - g_fall_flag_ms >= HM10_FALL_HOLD_MS))
      {
          g_fall_flag = 0;
      }

      /* 기준점을 '지금' 으로 갱신한다 (직전 기준 + 주기 가 아니다).
       * 어떤 이유로 한 주기를 놓쳐도 밀린 만큼 몰아서 보내지 않는다 —
       * 수신 측이 보는 것은 항상 '최신 상태' 하나면 충분하다. */
      if (now - g_last_vital_ms >= HM10_VITAL_PERIOD_MS)
      {
          g_last_vital_ms = now;
          HM10_Send_Now();
      }

      /* ------------------------------------------------------------------
       * [STAGE 4] 화면. 손목을 들었을 때만 켜지고, 켜져 있는 동안만 LVGL 이 돈다.
       * ------------------------------------------------------------------ */
      DisplayService_Service();

      /* ------------------------------------------------------------------
       * [STAGE 5] 처리할 일이 없으면 잠든다.
       *   센서 인터럽트(EXTI) 또는 DMA 완료가 곧 기상 신호다.
       *   SysTick 이 1ms 마다 깨우므로 화면이 켜져 있어도 LVGL 갱신 주기
       *   (LV_DEF_REFR_PERIOD 33ms)를 놓치지 않는다.
       * ------------------------------------------------------------------ */
      Enter_Idle();
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

/**
  * @brief FIFO 워터마크 인터럽트에 반응해 IMU 블록을 SPI DMA 로 수거 시작
  *
  * 적재량을 먼저 읽어 그만큼만 가져온다. 워터마크 설정값을 신뢰하지 않기 때문에
  * 레지스터 단위 해석이 어긋나도 판독 길이는 항상 실제와 일치한다.
  */
static void Collect_IMU_Block(void)
{
    /* ---- INT1 폴링 폴백 ----
     * 이전 아키텍처는 타이머 폴링이라 BMI270 INT 핀을 전혀 쓰지 않았다.
     * 따라서 PA5 가 물리적으로 배선돼 있지 않을 수 있다.
     * 인터럽트가 안 와도 시스템이 죽지 않도록 주기적으로 직접 수거한다.
     * 감지 성능은 동일하고, 절전 효율만 손해다. */
    if (g_bmi270_ok && !g_imu_fwm_pending && !g_imu_block_ready &&
        (HAL_GetTick() - g_last_imu_block_ms) >= IMU_FALLBACK_MS)
    {
        if (!g_imu_int_seen && !g_imu_fallback_warned)
        {
            g_imu_fallback_warned = 1;
            printf("[ IMU ] INT1(PA5) 인터럽트가 오지 않아 폴링 모드로 전환합니다.\r\n");
            printf("        BMI270 INT1 배선을 확인하세요 — 감지 기능은 정상 동작합니다.\r\n");
        }
        g_imu_fwm_pending = 1;
    }

    if (!g_imu_fwm_pending || !g_bmi270_ok) return;
    if (hspi2.State != HAL_SPI_STATE_READY) return;   // 직전 전송 진행 중이면 다음 턴에
    if (g_imu_block_ready) return;                    // 아직 소비되지 않은 블록이 있다

    g_imu_fwm_pending = 0;

    uint16_t fifo_bytes = 0;
    if (BMI270_GetFifoLength(&fifo_bytes) != HAL_OK)
    {
        SPI2_DMA_Reset_Unlock();
        return;
    }

    /* 프레임 경계로 내림 정렬 — 반쪽짜리 프레임을 넘기지 않는다. */
    fifo_bytes = (uint16_t)((fifo_bytes / BMI270_FIFO_FRAME_LEN) * BMI270_FIFO_FRAME_LEN);

    const uint16_t cap = BMI270_FIFO_MAX_FRAMES * BMI270_FIFO_FRAME_LEN;
    if (fifo_bytes > cap) fifo_bytes = cap;   // 남는 분량은 다음 인터럽트에서 마저 가져간다
    if (fifo_bytes == 0) return;

    g_imu_block_bytes = fifo_bytes;

    if (BMI270_StartFifoRead_DMA(bmi_fifo_rx, fifo_bytes) != HAL_OK)
    {
        g_imu_block_bytes = 0;
        SPI2_DMA_Reset_Unlock();
    }
}

static void Process_IMU_Block(void)
{
    if (!g_imu_block_ready) return;
    g_imu_block_ready = 0;

    uint16_t bytes = g_imu_block_bytes;
    g_imu_block_bytes = 0;
    g_last_imu_block_ms = HAL_GetTick();

    uint16_t frames = BMI270_ParseFifoBlock(bmi_fifo_rx, bytes,
                                            bmi_accel, BMI270_FIFO_MAX_FRAMES);
    if (frames == 0) return;

    /* 낙상 확정 게이트는 '광학적 접촉'(HasContact)까지만 요구한다.
     *
     * 원래는 IsWorn() —— 맥박까지 확인된 착용 —— 이었다. 그런데 이 센서로는
     * 손목에서 맥박이 잡히는 일이 드물어, 정작 사람이 차고 있는데도 게이트가
     * 열리지 않아 낙상이 통째로 묻혔다. 검출되지 않는 낙상보다는 가끔의 오보가
     * 낫다는 판단이다 (사람이 차고 있는 기기다).
     *
     * ⚠ 대가: HasContact 는 IR DC 와 반사율만 보므로 책상·바닥에 엎어둔
     *   상태에서도 참이 될 수 있다 (heart_rate_calc.c 상단 주석 참조).
     *   그 경우 충격 + 정지가 겹치면 오보가 나갈 수 있다. 다만 맥박이 30초간
     *   없으면 접촉 판정 자체가 해제되므로 창은 그만큼으로 제한된다. */
    FallDetection_ProcessBlock(bmi_accel, frames, HeartRateCalc_HasContact());

    /* 만보기는 착용 판정을 거치지 않는다 — IMU 만으로 성립하는 기능에
     * PPG 접촉 판정을 물리면 MAX30102 고장이 만보기까지 끌고 들어간다.
     * (상세 근거는 step_counter.c 상단 '착용 판정을 쓰지 않는 이유') */
    StepCounter_ProcessBlock(bmi_accel, frames);

    /* 손목 자세 판정 — 화면을 켤지 말지는 DisplayService 가 이 결과를 보고 정한다.
     * 만보기와 같은 이유로 착용 판정을 거치지 않는다. PPG 가 고장 나도 화면은
     * 켜져야 한다. */
    WristRaise_ProcessBlock(bmi_accel, frames);

    /* 버퍼 상한에 걸려 잘라 읽었다면 FIFO 에 아직 데이터가 남아있다.
     * INT 핀은 펄스 방식이라 새 엣지가 오지 않을 수 있으므로 스스로 재무장한다.
     * 이게 없으면 한 번 밀린 순간부터 IMU 스트림이 통째로 멈춘다. */
    if (bytes >= (BMI270_FIFO_MAX_FRAMES * BMI270_FIFO_FRAME_LEN))
    {
        g_imu_fwm_pending = 1;
    }
}

/**
  * @brief EXTI 를 놓쳐 멈춘 PPG 스트림을 되살린다.
  *
  * INT 핀이 눌린 채 남으면 하강 엣지가 오지 않아 스트림이 영구 정지한다.
  * 일정 시간 블록이 없으면 직접 수거를 걸어 복구한다.
  * FIFO_DATA 를 읽으면 A_FULL 이 해제되면서 INT 핀도 다시 풀린다.
  */
static void Collect_PPG_Block(void)
{
    if (!g_max30102_ok || g_ppg_block_ready) return;

    /* BUSY 가 오래 지속되면 정상 전송이 아니라 고착이다.
     * 여기서 조용히 return 만 하면 탈출구가 사라진다 (위 워치독 주석 참조). */
    if (hi2c1.State != HAL_I2C_STATE_READY)
    {
        if (g_i2c_busy_since_ms == 0)
        {
            g_i2c_busy_since_ms = HAL_GetTick();
        }
        else if ((HAL_GetTick() - g_i2c_busy_since_ms) >= I2C_BUSY_STUCK_MS)
        {
            g_i2c_busy_since_ms = 0;
            g_i2c_fault = 1;   /* 메인 루프의 Service_I2C_Fault() 가 핸들을 재구성한다 */
        }
        return;
    }
    g_i2c_busy_since_ms = 0;

    if ((HAL_GetTick() - g_last_ppg_block_ms) < PPG_FALLBACK_MS) return;

    /* 발생 횟수를 센다.
     *
     * 한 번만 경고하고 끝내면 '어쩌다 한 번' 인지 '계속 새는지' 를 알 수 없다.
     * 첫 발생과 이후 10회마다만 찍어 로그 오염 없이 빈도를 드러낸다.
     *
     * 원인은 크게 둘이다.
     *   · I2C 오류 후 복구      → g_i2c_recover_count 가 함께 증가
     *   · 순수 엣지 유실        → 이 카운트만 증가
     * 두 값을 비교하면 배선 문제인지 타이밍 문제인지 갈린다. */
    g_ppg_fallback_count++;

    if (g_ppg_fallback_count == 1 || (g_ppg_fallback_count % 10) == 0)
    {
        printf("[ PPG ] 인터럽트 유실 %lu회 (I2C 복구 %lu회) — 폴링으로 복구\r\n",
               (unsigned long)g_ppg_fallback_count,
               (unsigned long)g_i2c_recover_count);
    }

    /* 재시도 폭주를 막기 위해 성공 여부와 무관하게 타이머를 갱신한다 */
    g_last_ppg_block_ms = HAL_GetTick();

    if (MAX30102_StartBlockRead_DMA(ppg_rx, MAX30102_BLOCK_BYTES) != HAL_OK)
    {
        g_i2c_fault = 1;
    }
}

static void Process_PPG_Block(void)
{
    if (!g_ppg_block_ready) return;
    g_last_ppg_block_ms = HAL_GetTick();

    uint16_t samples = MAX30102_ParseBlock(ppg_rx, MAX30102_BLOCK_BYTES,
                                           ppg_samples, MAX30102_FIFO_DEPTH);

    /* A_FULL 플래그를 명시적으로 해제한다.
     * INT 핀이 액티브 로우로 눌린 채 남으면 EXTI 하강엣지가 다시 오지 않아
     * PPG 스트림이 첫 블록 이후 영구 정지한다. 2Hz 짜리 1바이트 판독이라
     * 비용은 무시할 수 있다. */
    uint8_t int_status = 0;
    MAX30102_ReadRegister(MAX30102_REG_INT_STAT_1, &int_status);

    if (samples > 0)
    {
        /* ⚠ 순서가 중요하다 —— 알고리즘이 먼저, 이득 조절이 나중이다.
         *
         * 지금 손에 있는 ppg_samples 는 '이득을 바꾸기 전' 전류로 찍힌 데이터다.
         * AutoGain 을 먼저 돌리면 NotifyGainChange() 가 파이프라인을 새 전류
         * 기준으로 맞춰놓은 상태에서 옛 전류 데이터를 밀어넣게 된다.
         * 그러면 DC 추정기가 옛 전류 값(예: 240000)으로 씨앗을 잡고, 바로 다음
         * 블록에서 새 전류 값(예: 157000)이 들어와 8만 카운트짜리 계단이 생긴다.
         * DC_ALPHA_NORMAL(차단 0.24Hz)로는 그 계단이 사라지는 데 8초가 걸리고,
         * 그동안 자기상관 창이 통째로 오염되어 r 이 0 에 머문다.
         *
         * 이 블록을 먼저 소비하면 이득 변경은 다음 블록부터 깔끔하게 적용된다. */
        HeartRateCalc_ProcessBlock(ppg_samples, samples, FallDetection_GetBlockMotion());

        /* 자동 이득 조절 — DC 를 목표 대역으로 끌어당긴다.
         *
         * 안정화 구간에는 건드리지 않는다. 착용 직후 첫 블록의 평균 DC 는
         * 공기 → 조직 계단 점프의 한복판 값이라, 그것으로 정한 이득은
         * 다음 블록에서 다시 고쳐야 한다. */
        if (!HeartRateCalc_IsSettling())
        {
            uint64_t ir_sum = 0;
            for (uint16_t i = 0; i < samples; i++) ir_sum += ppg_samples[i].ir;

            /* 반드시 HasContact() 여야 한다. IsWorn() 은 맥박 확인을 요구하는데,
             * 맥박 검출은 AGC 가 먼저 포화를 풀어줘야 가능하다 — 순환 의존이 된다. */
            if (MAX30102_AutoGain((uint32_t)(ir_sum / samples),
                                  HeartRateCalc_HasContact()))
            {
                HeartRateCalc_NotifyGainChange();
            }
        }
    }

    /* 블로킹 I2C 가 모두 끝난 뒤에야 플래그를 내린다.
     * g_ppg_block_ready 가 1 인 동안은 EXTI 가 새 DMA 를 걸지 않으므로,
     * 이 플래그 자체가 핸들 동시 접근을 막는 잠금 역할을 한다. */
    g_ppg_block_ready = 0;
}

/**
  * @brief 처리할 블록이 없을 때만 잠든다.
  *
  * 플래그 검사와 WFI 사이에 인터럽트가 끼어들면 이미 도착한 일감을 두고
  * 잠들 수 있다. PRIMASK 로 그 창을 닫는다 — WFI 는 PRIMASK 가 막아둔
  * 인터럽트로도 정상적으로 깨어나므로 취침 자체는 방해받지 않는다.
  */
static void Enter_Idle(void)
{
    __disable_irq();

    if (!g_imu_fwm_pending && !g_imu_block_ready && !g_ppg_block_ready && !g_i2c_fault)
    {
#if LOWPOWER_STOP_MODE
        HAL_SuspendTick();
        HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
        SystemClock_Config();          // Stop 진입 시 정지한 HSE/PLL 복구
        HAL_ResumeTick();
#else
        __WFI();
#endif
    }

    __enable_irq();
}

// 현재 바이탈 + 낙상 플래그 + 걸음 수를 HM-10 7바이트 패킷으로 송신
static void HM10_Send_Now(void)
{
    uint32_t bpm  = HeartRateCalc_GetBPM();
    uint32_t spo2 = HeartRateCalc_GetSpO2();
    uint8_t  hr   = (bpm > 255) ? 255 : (uint8_t)bpm;   /* 심박 8bit 클램프 */

    /* 걸음 수 16bit 포화.
     * 자정마다 0 으로 돌아가므로 65535 를 넘길 일은 없지만, 시각 동기가 없어
     * 자정 리셋이 걸리지 않은 채 며칠 켜져 있으면 도달할 수 있다. 그때
     * 잘라내지 않으면 65536 에서 0 으로 되감겨 '갑자기 안 걸은 것' 처럼 보인다. */
    uint32_t steps = StepCounter_GetSteps();
    uint16_t steps16 = (steps > 65535U) ? 65535U : (uint16_t)steps;

    hm10_send_packet(hr, (uint8_t)spo2, g_fall_flag, steps16);
}

/* 낙상 확정 시 호출 (fall_detection.c) - 플래그 세우고 즉시 1회 송신 */
void Send_Fall_Alert_Hardware(void)
{
    g_fall_flag = HM10_FALL_SUSPECT;
    g_fall_flag_ms = HAL_GetTick();
    HM10_Send_Now();   // 다음 주기 기다리지 않고 즉시 반영

    /* 주기 송신 기준점도 함께 민다.
     * 안 그러면 주기 만료 직전에 낙상이 뜰 때 같은 내용의 패킷이
     * 수십 ms 간격으로 두 번 나간다. 수신 측에서 중복 이벤트로 보인다. */
    g_last_vital_ms = HAL_GetTick();
}

// 부팅 시 실패를 LED 깜빡임 횟수로 알린다 (USB 미연결 시 유일한 단서)
//   2회 = BMI270 초기화 실패, 3회 = MAX30102 초기화 실패, 4회 = BMI270 FIFO/INT 구성 실패
// 부팅 때 한 번만 도는 유한 루프다. 여기서 갇히면 안 된다.
static void Blink_Error_Code(uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        HAL_GPIO_WritePin(STATUS_LED_GPIO_Port, STATUS_LED_Pin, GPIO_PIN_RESET);
        HAL_Delay(200);
        HAL_GPIO_WritePin(STATUS_LED_GPIO_Port, STATUS_LED_Pin, GPIO_PIN_SET);
        HAL_Delay(200);
    }
    HAL_Delay(600);   // 다음 코드와 구분되게 쉬어간다
}

/**
  * @brief  독립 워치독 기동. 한 번 켜면 리셋 전까지 끌 수 없다.
  *
  * ⚠ 반드시 부팅 시퀀스가 끝난 뒤에 부를 것.
  *   Blink_Error_Code() 는 HAL_Delay 로 최대 2.2초를 쓰고, 센서 초기화
  *   재시도까지 겹치면 그보다 길어진다. 그 구간에서 켜면 부팅이 끝나기 전에
  *   리셋이 걸려 재부팅 루프가 된다.
  */
static void IWDG_Start(void)
{
    /* 순서가 중요하다 — 기동이 먼저다.
     *
     * IWDG 는 전용 LSI 로 도는데, KR=0xCCCC 를 써야 그 LSI 가 켜진다.
     * PR/RLR 을 먼저 쓰고 SR 을 기다리면 아직 클럭이 없어 PVU/RVU 가
     * 영영 내려가지 않는다. HAL_IWDG_Init() 도 같은 이유로 기동을 앞에 둔다.
     * 기동 직후 잠깐은 기본값(약 0.5초)으로 도는데, 아래 재설정이
     * 수 마이크로초 안에 끝나므로 문제되지 않는다. */
    IWDG->KR  = IWDG_KEY_ENABLE;      /* 기동 + LSI 시동 */
    IWDG->KR  = IWDG_KEY_UNLOCK;      /* PR/RLR 쓰기 잠금 해제 */
    IWDG->PR  = IWDG_PR_PR;           /* 분주비 256 (최대) */
    IWDG->RLR = IWDG_RELOAD_VAL;

    /* 새 값이 반영될 때까지 기다린다. 무한 대기는 하지 않는다 —
     * 워치독을 켜려다 여기서 멈추면 본말이 전도된다. */
    uint32_t guard = 100000U;
    while ((IWDG->SR != 0U) && (--guard != 0U)) { }

    IWDG->KR = IWDG_KEY_RELOAD;       /* 새 리로드값으로 카운터 적재 */
}

// SPI 데드락 탈출
void SPI2_DMA_Reset_Unlock(void)
{
    HAL_SPI_DMAStop(&hspi2);
    __HAL_SPI_DISABLE(&hspi2);
    __HAL_SPI_ENABLE(&hspi2);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
}

/* 센서 하드웨어 인터럽트 —— 이 시스템의 유일한 기상 신호
 *   PB0 : MAX30102 FIFO Almost Full (25샘플 = 0.5초)
 *   PA5 : BMI270 INT1 = FIFO 워터마크
 *   PA6 : BMI270 INT2 = any-motion (절전 게이트)
 *
 * ISR 안에서는 블로킹 판독을 하지 않는다. FIFO 길이 조회가 필요한 IMU 쪽은
 * 플래그만 세우고 실제 수거는 메인 루프로 넘긴다. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    switch (GPIO_Pin)
    {
        case MAX30102_INT_Pin:
            /* FIFO_DATA 연속 판독이 RD_PTR 전진과 인터럽트 해제를 겸한다.
             *
             * hi2c1.State 확인이 핵심이다. 메인 루프가 블로킹 판독 중일 때
             * 여기서 DMA 를 걸면 같은 핸들을 양쪽에서 만지게 되어 상태 머신이 깨진다.
             * 버스 복구는 절대 여기서 하지 않는다 — HAL_Delay 가 ISR 을 영구 정지시킨다. */
            if (!g_ppg_block_ready && g_max30102_ok &&
                hi2c1.State == HAL_I2C_STATE_READY)
            {
                if (MAX30102_StartBlockRead_DMA(ppg_rx, MAX30102_BLOCK_BYTES) != HAL_OK)
                {
                    g_i2c_fault = 1;
                }
            }
            break;

        case BMI270_INT1_Pin:
            g_imu_int_seen = 1;
            g_imu_fwm_pending = 1;
            break;

        case BMI270_INT2_Pin:
            /* any-motion 은 절전 게이트 용도다. FIFO 워터마크가 감지를 담당하므로
             * 여기서 할 일은 없다 — 인터럽트를 소비해 EXTI 를 정리하는 것으로 충분하다. */
            break;

        default:
            break;
    }
}

// I2C 블록 수신 완료 (MAX30102)
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        g_ppg_block_ready = 1;
    }
}

/* I2C 오류 콜백은 인터럽트 컨텍스트에서 실행된다.
 *
 * 여기서 복구를 직접 수행하면 안 된다. I2C1_BusRecovery() 는 HAL_Delay() 를 쓰는데,
 * 이 ISR 은 우선순위 1 이고 SysTick 은 15 라서 틱이 영원히 증가하지 못한다.
 * 즉 HAL_Delay 가 반환되지 않고 시스템이 그 자리에서 멈춘다.
 * 따라서 플래그만 세우고 실제 복구는 메인 루프에 맡긴다. */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        /* ⚠ 여기서 g_ppg_block_ready 를 내리면 안 된다.
         *
         * 이 플래그는 hi2c1 핸들의 잠금을 겸한다. Process_PPG_Block() 이
         * 블로킹 쓰기를 하는 도중 ISR 에서 잠금을 풀면 EXTI 가 같은 핸들에
         * DMA 를 걸 수 있다. 해제는 소유자인 Process_PPG_Block() 에서만 한다. */
        g_i2c_fault = 1;
    }
}

/**
  * @brief  I2C 오류 복구 (메인 루프 전용 — HAL_Delay 사용)
  */
static void Service_I2C_Fault(void)
{
    if (!g_i2c_fault) return;
    g_i2c_fault = 0;
    g_i2c_recover_count++;   /* PPG 폴백 로그에서 원인 구분에 쓰인다 */

    HAL_I2C_DeInit(&hi2c1);
    I2C1_BusRecovery();
    MX_I2C1_Init();

    /* 주변장치가 BUSY 를 물고 있으면 SWRST 로만 풀린다 */
    if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY))
    {
        hi2c1.Instance->CR1 |=  I2C_CR1_SWRST;
        hi2c1.Instance->CR1 &= ~I2C_CR1_SWRST;
        HAL_I2C_Init(&hi2c1);
    }

    /* ------------------------------------------------------------------
     * 버스를 고쳤다고 센서가 살아난 것은 아니다.
     *
     * 충격이나 전원 드룹은 I2C 오류와 센서 자체 리셋을 동시에 일으킨다.
     * 위까지는 MCU 쪽 버스만 재건할 뿐이라, 센서가 MODE_CONF=0x00 으로
     * 돌아가 있으면 LED 가 꺼진 채 FIFO 도 채우지 않는다. 그런데 버스는
     * 정상이므로 더 이상 오류가 나지 않고, 아무도 이 상태를 깨우지 않는다.
     * 실측: 충격 직후 측정이 멈춘 뒤 재부팅 전까지 복구되지 않았다.
     *
     * 낙상 감지 웨어러블에서 이건 치명적이다 —— 낙상은 곧 충격이고,
     * HeartRateCalc_IsWorn() 이 맥박 기반이라 PPG 가 죽으면 낙상 판정까지
     * 함께 꺼진다. 정확히 필요한 순간에 기능이 사라진다.
     * ------------------------------------------------------------------ */
    if (!g_max30102_ok) return;

    /* 재초기화 폭주 방지. 센서가 물리적으로 빠졌다면 매 오류마다 50ms 짜리
     * 초기화를 시도하게 되고, 그러면 메인 루프가 그것만 하다 끝난다. */
    if ((HAL_GetTick() - g_last_ppg_reinit_ms) < PPG_REINIT_COOLDOWN_MS) return;

    if (MAX30102_IsAlive()) return;   /* 설정이 살아 있으면 건드리지 않는다 */

    g_last_ppg_reinit_ms = HAL_GetTick();
    g_ppg_reinit_count++;

    printf("[ PPG ] 센서 설정 소실 감지 — 재초기화 (%lu회차)\r\n",
           (unsigned long)g_ppg_reinit_count);

    if (MAX30102_Init() == HAL_OK)
    {
        /* 센서를 새로 세웠으니 신호 파이프라인도 처음부터다.
         * 필터 상태와 자기상관 이력에는 죽기 직전의 잔재가 남아 있고,
         * 그걸 새 신호와 한 창에 섞으면 가짜 주기가 만들어진다. */
        HeartRateCalc_Reset();
        g_last_ppg_block_ms = HAL_GetTick();
        printf("[ PPG ] 재초기화 성공 — 측정 재개\r\n");
    }
    else
    {
        /* 실패해도 g_max30102_ok 를 내리지 않는다. 접촉 불량은 대개
         * 일시적이라, 쿨다운 뒤 다음 오류에서 다시 시도하는 편이 낫다. */
        printf("[ PPG ] 재초기화 실패 — 다음 오류에서 재시도\r\n");
    }
}

/**
  * @brief  물린 I2C 버스를 물리 계층에서 강제로 풀어낸다.
  *
  * SDA 가 Low 로 잡혀 있는 동안 SCL 을 최대 9번 토글해 슬레이브가 남은 비트를
  * 모두 내보내게 한 뒤, STOP 조건을 만들어 트랜잭션을 종료시킨다.
  * 9번인 이유는 한 바이트(8비트) + ACK 를 빠져나오기에 충분하기 때문이다.
  *
  * @note  MX_I2C1_Init() 보다 먼저 불러야 한다. 핀이 아직 AF 로 넘어가기 전에
  *        GPIO 로 직접 흔들어야 하기 때문이다.
  */
static void I2C1_BusRecovery(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB6=SCL, PB7=SDA 를 오픈드레인 GPIO 로 되돌린다 */
    gpio.Pin   = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* 둘 다 해제(High)에서 출발 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
    HAL_Delay(1);

    /* SDA 가 물려 있는 동안만 클럭을 넣는다 */
    for (uint8_t i = 0; i < 9; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET) break;  // 이미 풀렸다

        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        HAL_Delay(1);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        HAL_Delay(1);
    }

    /* STOP 조건 생성: SCL 이 High 인 상태에서 SDA 를 Low → High */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    HAL_Delay(1);

    /* 핀을 아날로그로 되돌려 둔다. 곧 MX_I2C1_Init() 이 AF 로 다시 잡는다. */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
}

// SPI 블록 수신 완료 (BMI270 FIFO)
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
        g_imu_block_ready = 1;
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_SPI_TxRxCpltCallback(hspi);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        g_imu_block_ready = 0;
        g_imu_block_bytes = 0;
        SPI2_DMA_Reset_Unlock();
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
