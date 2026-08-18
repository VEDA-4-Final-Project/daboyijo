#ifndef APP_DRIVERS_BMI270_H_
#define APP_DRIVERS_BMI270_H_

#include "main.h"  // HAL 라이브러리 및 핀 맵 매핑용 필수 인클루드

/* --- [ BMI270 레지스터 주소 맵 ] --- */
#define BMI270_REG_CHIP_ID       0x00   // 칩 ID 확인 레지스터
#define BMI270_REG_INT_STATUS_0  0x1C   // 피처 엔진 인터럽트 상태 (any-motion 등)
#define BMI270_REG_INT_STATUS_1  0x1D   // 데이터 인터럽트 상태 (FIFO watermark/full)
#define BMI270_REG_INTERNAL_STAT 0x21   // 내부 동작 상태 확인 레지스터
#define BMI270_REG_FIFO_LENGTH_0 0x24   // FIFO 적재 바이트 수 (LSB)
#define BMI270_REG_FIFO_LENGTH_1 0x25   // FIFO 적재 바이트 수 (MSB, 상위 6비트만 유효)
#define BMI270_REG_FIFO_DATA     0x26   // FIFO 버스트 판독 창구
#define BMI270_REG_FEAT_PAGE     0x2F   // 피처 엔진 페이지 선택
#define BMI270_REG_FEATURES      0x30   // 피처 엔진 16바이트 창구 시작 (0x30~0x3F)
#define BMI270_REG_INIT_CTRL     0x59   // 초기화 제어 레지스터
#define BMI270_REG_PWR_CONF      0x7C   // 전원 설정 레지스터
#define BMI270_REG_PWR_CTRL      0x7D   // 전원 제어 레지스터
#define BMI270_REG_CMD           0x7E   // 커맨드 레지스터 (FIFO flush / soft reset)

#define BMI270_REG_INIT_ADDR_0   0x5B   // 펌웨어 스트림 주소 지정 (LSB)
#define BMI270_REG_INIT_ADDR_1   0x5C   // 펌웨어 스트림 주소 지정 (MSB)
#define BMI270_REG_INIT_DATA     0x5E   // 설정 데이터 스트림 입력 레지스터

#define BMI270_REG_ACC_X_LSB     0x0C   // 가속도 데이터 시작 주소 (X축 LSB)
#define BMI270_REG_ACC_CONF      0x40   // 가속도 ODR 및 대역폭 설정
#define BMI270_REG_ACC_RANGE     0x41   // 가속도 측정 범위 설정
#define BMI270_REG_GYR_X_LSB     0x12   // 자이로 데이터 시작 주소 (X축 LSB)
#define BMI270_REG_GYR_CONF      0x42   // 자이로 ODR 및 대역폭 설정
#define BMI270_REG_GYR_RANGE     0x43   // 자이로 측정 범위 설정

#define BMI270_REG_FIFO_DOWNS    0x45   // FIFO 다운샘플링 설정
#define BMI270_REG_FIFO_WTM_0    0x46   // FIFO 워터마크 (LSB)
#define BMI270_REG_FIFO_WTM_1    0x47   // FIFO 워터마크 (MSB)
#define BMI270_REG_FIFO_CONFIG_0 0x48   // FIFO 동작 모드 (stop_on_full / time_en)
#define BMI270_REG_FIFO_CONFIG_1 0x49   // FIFO 데이터 소스 선택 (acc/gyr/aux/header)

#define BMI270_REG_INT1_IO_CTRL  0x53   // INT1 핀 전기적 특성
#define BMI270_REG_INT2_IO_CTRL  0x54   // INT2 핀 전기적 특성
#define BMI270_REG_INT1_MAP_FEAT 0x56   // INT1 <- 피처 엔진 인터럽트 매핑
#define BMI270_REG_INT2_MAP_FEAT 0x57   // INT2 <- 피처 엔진 인터럽트 매핑
#define BMI270_REG_INT_MAP_DATA  0x58   // INT1/INT2 <- 데이터 인터럽트 매핑

/* --- [ 레지스터 비트 상수 ] --- */
#define BMI270_CHIP_ID_VAL       0x24   // BMI270 고유 식별자 값

#define BMI270_CMD_FIFO_FLUSH    0xB0   // FIFO 강제 비우기 커맨드
#define BMI270_CMD_SOFT_RESET    0xB6   // 소프트 리셋 커맨드

/* FIFO_CONFIG_1 비트 (bmi2_defs.h 의 16비트 마스크 상위 바이트와 동일) */
#define BMI270_FIFO_HEADER_EN    0x10
#define BMI270_FIFO_AUX_EN       0x20
#define BMI270_FIFO_ACC_EN       0x40
#define BMI270_FIFO_GYR_EN       0x80

/* INT_MAP_DATA 비트 — 하위 니블이 INT1, 상위 니블이 INT2 */
#define BMI270_INT1_FFULL        0x01
#define BMI270_INT1_FWM          0x02
#define BMI270_INT1_DRDY         0x04

/* INTx_MAP_FEAT 비트 */
#define BMI270_INT_ANY_MOT       0x40   // bmi270.h: BMI270_INT_ANY_MOT_MASK

/* INT_STATUS_0 비트 */
#define BMI270_STATUS_ANY_MOT    0x40   // bmi270.h: BMI270_ANY_MOT_STATUS_MASK

/* INT_STATUS_1 비트 */
#define BMI270_STATUS_FWM        0x40
#define BMI270_STATUS_FFULL      0x01

/* INTx_IO_CTRL: output_en(bit3) | lvl=active-high(bit1) → push-pull, 상승엣지 */
#define BMI270_INT_IO_PUSHPULL_AH  0x0A

/* --- [ 동작 설정 상수 ] --- */
/* 가속도 ±16g. 낙상 충격은 3~6g에 달하므로 ±2g로는 축마다 클리핑되어
 * 충격 크기를 신뢰할 수 없다. 낙상 판별의 전제 조건이라 범위를 넓게 잡는다. */
#define BMI270_ACC_RANGE_16G     0x03
#define BMI270_ACC_SENS_LSB_PER_G 2048.0f   // 32768 / 16

/* 자이로 ±2000dps */
#define BMI270_GYR_RANGE_2000DPS 0x00
#define BMI270_GYR_SENS_LSB_PER_DPS 16.4f

/* 가속도 ODR 100Hz (acc_filter_perf=1, bwp=normal(010), odr=0x08)
 * 충격 파형을 놓치지 않으려면 낙상 판별에는 100Hz가 하한선이다. */
#define BMI270_ACC_CONF_100HZ    0xA8
#define BMI270_ACC_ODR_HZ        100
#define BMI270_GYR_CONF_100HZ    0xA8

/* --- [ FIFO 블록 파라미터 ] --- */
/* 헤더리스 + 가속도 단독 → 프레임당 정확히 6바이트, 순서 모호성 없음.
 * (acc+gyr 동시 적재 시 헤더리스 프레임 내부 순서가 문서상 불명확해
 *  판독 오정렬 위험이 있어 의도적으로 가속도만 적재한다. 자이로는 필요 시
 *  BMI270_ReadGyro() 로 직접 버스트 판독.) */
#define BMI270_FIFO_FRAME_LEN    6
#define BMI270_FIFO_BLOCK_FRAMES 50                  // 0.5초 @100Hz
#define BMI270_FIFO_WTM_BYTES    (BMI270_FIFO_BLOCK_FRAMES * BMI270_FIFO_FRAME_LEN)

/* DMA 수신 버퍼는 워터마크 초과분과 SPI 선두 오버헤드(주소1 + 더미1)를 흡수해야 한다. */
#define BMI270_FIFO_MAX_FRAMES   80
#define BMI270_SPI_HDR_LEN       2
#define BMI270_FIFO_RX_LEN       (BMI270_FIFO_MAX_FRAMES * BMI270_FIFO_FRAME_LEN + BMI270_SPI_HDR_LEN)

/* --- [ 데이터 구조체 ] --- */
typedef struct {
    float x;
    float y;
    float z;
} BMI270_Data_t;

/* --- [ 하위 레벨 통신 API ] --- */
HAL_StatusTypeDef BMI270_ReadRegister(uint8_t reg, uint8_t *val);
HAL_StatusTypeDef BMI270_WriteRegister(uint8_t reg, uint8_t val);

/* --- [ 상위 레벨 응용 API ] --- */
HAL_StatusTypeDef BMI270_Init(void);
HAL_StatusTypeDef BMI270_Calibrate_Gyro(BMI270_Data_t *bias);
HAL_StatusTypeDef BMI270_ReadGyro(BMI270_Data_t *gyro, const BMI270_Data_t *bias);

/* --- [ FIFO / 인터럽트 구성 ] --- */
HAL_StatusTypeDef BMI270_ConfigFifoAndInterrupts(void);
HAL_StatusTypeDef BMI270_ConfigAnyMotion(uint16_t threshold_mg, uint16_t duration_ms);
HAL_StatusTypeDef BMI270_FifoFlush(void);
HAL_StatusTypeDef BMI270_GetFifoLength(uint16_t *bytes);

/* --- [ FIFO 블록 수거 및 해석 ] --- */
HAL_StatusTypeDef BMI270_StartFifoRead_DMA(uint8_t *rx_buf, uint16_t data_bytes);
uint16_t BMI270_ParseFifoBlock(const uint8_t *rx_buf, uint16_t data_bytes,
                               BMI270_Data_t *out, uint16_t max_frames);

#endif /* APP_DRIVERS_BMI270_H_ */
