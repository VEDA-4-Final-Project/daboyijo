#include "bmi270.h"
#include "bmi270_config.h"
#include <stdio.h>
#include <string.h>

extern SPI_HandleTypeDef hspi2;

/* 8KB 컨피그 업로드 구간에서만 쓰는 저속 클럭.
 * 점퍼선/브레드보드 배선에서는 6MHz 스트림이 쉽게 깨지고, 그 결과가
 * 정확히 INTERNAL_STATUS = 0x02 (init_err) 로 나타난다.
 * 업로드는 부팅 때 딱 한 번이라 느려도 손해가 없다. */
#define BMI270_SPI_PRESC_CONFIG  SPI_BAUDRATEPRESCALER_64   // 48MHz/64 = 750kHz
#define BMI270_SPI_PRESC_RUN     SPI_BAUDRATEPRESCALER_8    // 48MHz/8  = 6MHz

static void bmi270_spi_set_prescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(&hspi2);
    hspi2.Init.BaudRatePrescaler = prescaler;
    MODIFY_REG(hspi2.Instance->CR1, SPI_CR1_BR, prescaler);
    __HAL_SPI_ENABLE(&hspi2);
}

/* 소프트 리셋 사용 여부.
 * 0 으로 두면 리셋 없이 예전 시퀀스 그대로 동작한다. 리셋이 문제인지
 * 배선이 문제인지 가를 때 이 스위치 하나만 뒤집으면 된다. */
#define BMI270_USE_SOFT_RESET  1

/**
  * @brief  칩 ID 를 최대 attempts 회까지 재시도하며 읽는다.
  *
  * BMI270 은 전원 인가 직후 I2C 모드로 시작하고 CSB 상승엣지를 봐야 SPI 로
  * 전환된다. 그래서 첫 판독은 쓰레기값이 나오는 게 정상이다. 한 번 읽고
  * 판단하면 멀쩡한 칩을 불량으로 오진한다.
  */
static HAL_StatusTypeDef bmi270_read_chip_id(uint8_t *id, uint8_t attempts)
{
    for (uint8_t i = 0; i < attempts; i++)
    {
        if (BMI270_ReadRegister(BMI270_REG_CHIP_ID, id) != HAL_OK) return HAL_ERROR;
        if (*id == BMI270_CHIP_ID_VAL) return HAL_OK;
        HAL_Delay(2);
    }
    return HAL_ERROR;   // 마지막으로 읽은 값은 *id 에 그대로 남겨 진단에 쓴다
}

/* INTERNAL_STATUS.message 해석 (하위 4비트) */
static const char *bmi270_status_str(uint8_t msg)
{
    switch (msg & 0x0F)
    {
        case 0x00: return "not_init (컨피그 업로드가 시작조차 안 됨)";
        case 0x01: return "init_ok";
        case 0x02: return "init_err (컨피그 스트림 손상 — SPI 신호 품질/배선 의심)";
        case 0x03: return "drv_err";
        case 0x04: return "sns_stop";
        case 0x05: return "nvm_error";
        case 0x06: return "start_up_error (전원 상승 문제)";
        case 0x07: return "compat_error (칩과 컨피그 파일 불일치)";
        default:   return "unknown";
    }
}

/**
  * @brief  BMI270 레지스터 1바이트 읽기 (SPI 프로토콜: 주소 + 더미 + 데이터 = 총 3바이트)
  */
HAL_StatusTypeDef BMI270_ReadRegister(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return HAL_ERROR;

    uint8_t tx_buf[3] = { (uint8_t)(reg | 0x80), 0x00, 0x00 };
    uint8_t rx_buf[3] = { 0 };

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status == HAL_OK)
    {
        *val = rx_buf[2]; // BMI270 SPI 특성상 3번째 바이트에 실제 데이터가 안착함
    }
    return status;
}

/**
  * @brief  BMI270 레지스터 1바이트 쓰기
  */
HAL_StatusTypeDef BMI270_WriteRegister(uint8_t reg, uint8_t val)
{
    uint8_t tx_buf[2] = { (uint8_t)(reg & 0x7F), val };

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi2, tx_buf, 2, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    return status;
}

/**
  * @brief  BMI270 메모리 초기화 및 펌웨어 스트리밍 구성 함수
  */
HAL_StatusTypeDef BMI270_Init(void)
{
    uint8_t chip_id = 0;
    uint8_t internal_stat = 0;

    printf("=== BMI270 Clean Initialization ===\r\n");

    bmi270_spi_set_prescaler(BMI270_SPI_PRESC_RUN);

    /* 0-a. 리셋을 건드리기 전 상태를 먼저 남긴다.
     *      여기서 0x24 가 나오면 배선과 SPI 는 정상이라는 뜻이고,
     *      그 뒤에 실패한다면 원인은 리셋 시퀀스 쪽으로 좁혀진다.
     *      (정상 동작이 확인되어 출력은 껐다. 초기화 실패를 다시 디버깅해야
     *       하면 아래 두 printf 의 주석을 풀면 된다.) */
    uint8_t id_before = 0;
    (void)bmi270_read_chip_id(&id_before, 5);
    // printf("[ BMI270 ] 리셋 전 CHIP_ID = 0x%02X\r\n", id_before);

#if BMI270_USE_SOFT_RESET
    /* 0-b. 소프트 리셋으로 항상 같은 출발점을 만든다.
     *      이게 없으면 재시도가 반쯤 초기화된 상태에서 시작해 매번 똑같이 실패한다.
     *      리셋 직후 칩은 I2C 모드로 돌아가므로 CSB 상승엣지를 태워
     *      SPI 모드로 되돌려야 한다 (더미 판독의 실제 목적). */
    if (BMI270_WriteRegister(BMI270_REG_CMD, BMI270_CMD_SOFT_RESET) != HAL_OK) return HAL_ERROR;
    HAL_Delay(10);

    uint8_t id_after = 0;
    (void)bmi270_read_chip_id(&id_after, 5);
    // printf("[ BMI270 ] 리셋 후 CHIP_ID = 0x%02X\r\n", id_after);
#endif

    // 1. 칩 ID 확인 및 통신 신뢰성 점검
    if (bmi270_read_chip_id(&chip_id, 5) != HAL_OK)
    {
        printf("[ FAIL ] Sensor Not Found! (ID: 0x%02X)\r\n", chip_id);
        if (chip_id == 0x00) {
            printf("         → MISO 가 계속 Low. 배선 단선/미결선 또는 센서 무전원 의심\r\n");
        } else if (chip_id == 0xFF) {
            printf("         → MISO 가 계속 High. CS 미동작 또는 센서 무응답 의심\r\n");
        }
        return HAL_ERROR;
    }

    // 2. 절전 모드 해제 및 구성 데이터 업로드 준비
    if (BMI270_WriteRegister(BMI270_REG_PWR_CONF, 0x00) != HAL_OK) return HAL_ERROR;
    HAL_Delay(2);
    if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x00) != HAL_OK) return HAL_ERROR;
    HAL_Delay(2);

    /* 업로드 구간만 저속으로 내린다 */
    bmi270_spi_set_prescaler(BMI270_SPI_PRESC_CONFIG);

    // 3. 256바이트 블록 단위로 펌웨어 데이터 구성 스트리밍 (총 8192바이트)
    for (uint16_t i = 0; i < 8192; i += 256)
    {
        // BMI270 내부 설정 버스트 주소는 16비트(Word) 단위로 카운트됨
        uint16_t word_addr = i / 2;

        // 주소 지정 레지스터 로드 (하위 4비트 및 상위 8비트 분할 분배)
        if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_0, (uint8_t)(word_addr & 0x0F)) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_INIT_ADDR_1, (uint8_t)((word_addr >> 4) & 0xFF)) != HAL_OK) return HAL_ERROR;

        uint8_t reg_init_data = BMI270_REG_INIT_DATA;
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);

        // 레지스터 쓰기 명령 전송
        if (HAL_SPI_Transmit(&hspi2, &reg_init_data, 1, 10) != HAL_OK)
        {
            HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
            return HAL_ERROR;
        }

        // 💡 [수정] const 한정자 폐기 경고(Discarded-qualifiers) 방지를 위한 깔끔한 데이터 주소 캐스팅 적용
        uint8_t *p_stream_chunk = (uint8_t *)&bmi270_config_file[i];
        if (HAL_SPI_Transmit(&hspi2, p_stream_chunk, 256, 100) != HAL_OK)
        {
            HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
            return HAL_ERROR;
        }

        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
        HAL_Delay(1);
    }

    /* 업로드 완료 — 운전 속도로 복귀 */
    bmi270_spi_set_prescaler(BMI270_SPI_PRESC_RUN);

    // 4. 초기화 제어 시퀀스 종료 및 디바이스 가동 대기
    if (BMI270_WriteRegister(BMI270_REG_INIT_CTRL, 0x01) != HAL_OK) return HAL_ERROR;

    /* 5. ASIC 내부 코어 로드 완료를 폴링으로 기다린다.
     *    고정 30ms 대기는 근거가 없다. 데이터시트상 통상 20ms 안팎이지만
     *    개체·전압에 따라 흔들리므로 최대 200ms 까지 상태를 직접 확인한다. */
    uint32_t start_ms = HAL_GetTick();
    do
    {
        HAL_Delay(5);
        if (BMI270_ReadRegister(BMI270_REG_INTERNAL_STAT, &internal_stat) != HAL_OK) return HAL_ERROR;
    }
    while (((internal_stat & 0x0F) != 0x01) && ((HAL_GetTick() - start_ms) < 200));

    /* message 필드(하위 4비트)만 본다. 상위 비트에는 axes_remap_error 등
     * 초기화 성공 여부와 무관한 플래그가 실릴 수 있다. */
    if ((internal_stat & 0x0F) == 0x01)
    {
        printf("[ SUCCESS ] BMI270 Ready!\r\n");

        /* 가속도계만 켠다 (bit2=acc_en). 자이로(bit1)는 끈 채로 둔다.
         *
         * 낙상 판별은 SVM(가속도 벡터 크기) 기반이고, PPG 모션 블랭킹도
         * 가속도 블록의 표준편차를 쓴다. FIFO 에도 가속도만 적재하므로
         * 자이로 데이터를 읽는 곳이 한 군데도 없다.
         * BMI270 자이로는 약 900uA 를 상시 소모한다 — 가속도(약 150uA)의 6배다.
         * 초절전이 목표인 이상 쓰지 않는 센서를 켜둘 이유가 없다.
         *
         * 나중에 자이로가 필요해지면 0x06 으로 되돌리고 BMI270_ReadGyro() 를
         * 호출하면 된다. 드라이버 함수는 그대로 남겨두었다. */
        if (BMI270_WriteRegister(BMI270_REG_PWR_CTRL, 0x04) != HAL_OK) return HAL_ERROR;
        HAL_Delay(10);

        // FIFO 블록 아키텍처용 ODR 100Hz 통일 (충격 파형 보존을 위한 하한선)
        if (BMI270_WriteRegister(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_100HZ) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_GYR_CONF, BMI270_GYR_CONF_100HZ) != HAL_OK) return HAL_ERROR;
        HAL_Delay(10);

        /* 하드웨어 풀스케일 범위 지정 (가속도: ±16g, 자이로: ±2000dps)
         * ±2g 로는 낙상 충격(3~6g)이 축마다 포화되어 충격 크기 자체를 신뢰할 수 없다.
         * 임계값 비교가 성립하려면 범위부터 넓어야 한다. */
        if (BMI270_WriteRegister(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_16G) != HAL_OK) return HAL_ERROR;
        if (BMI270_WriteRegister(BMI270_REG_GYR_RANGE, BMI270_GYR_RANGE_2000DPS) != HAL_OK) return HAL_ERROR;
        HAL_Delay(50);

        /* adv_power_save 는 끄고 fifo_self_wake_up 만 켠다.
         * 절전 모드가 걸린 상태에서는 FIFO 판독이 간헐적으로 실패한다. */
        if (BMI270_WriteRegister(BMI270_REG_PWR_CONF, 0x02) != HAL_OK) return HAL_ERROR;
        HAL_Delay(10);

        return HAL_OK;
    }
    else
    {
        printf("[ FAIL ] Init Failed! Internal Status: 0x%02X → %s\r\n",
               internal_stat, bmi270_status_str(internal_stat));
        return HAL_ERROR;
    }
}

/**
  * @brief  자이로스코프 정적 영점 오프셋(Bias) 측정 캘리브레이션 함수
  */
HAL_StatusTypeDef BMI270_Calibrate_Gyro(BMI270_Data_t *bias)
{
    if (bias == NULL) return HAL_ERROR;
    printf("[INFO] Calibrating Gyroscope... Keep the device still.\r\n");

    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    const int sample_count = 100;

    // SPI 버스트 읽기 패킷 배열 구성 (명령어 1B + 더미 1B + 데이터 6B = 총 8바이트)
    uint8_t tx_buf[8] = { 0 };
    uint8_t rx_buf[8] = { 0 };
    tx_buf[0] = (uint8_t)(BMI270_REG_GYR_X_LSB | 0x80);

    for (int i = 0; i < sample_count; i++)
    {
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
        HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

        if (status != HAL_OK) return HAL_ERROR;

        // 패킷 매핑 해제 (rx_buf[2]부터 유효 바이트 할당됨)
        int16_t raw_x = (int16_t)(((uint16_t)rx_buf[3] << 8) | rx_buf[2]);
        int16_t raw_y = (int16_t)(((uint16_t)rx_buf[5] << 8) | rx_buf[4]);
        int16_t raw_z = (int16_t)(((uint16_t)rx_buf[7] << 8) | rx_buf[6]);

        sum_x += raw_x;
        sum_y += raw_y;
        sum_z += raw_z;

        HAL_Delay(10); // 자이로 ODR 100Hz 레이트에 매칭
    }

    // ±2000dps 설정 하에서의 LSB 감도 계수 (16.4 LSB/dps) 기반 변환 및 평균 산출
    bias->x = ((float)sum_x / (float)sample_count) / BMI270_GYR_SENS_LSB_PER_DPS;
    bias->y = ((float)sum_y / (float)sample_count) / BMI270_GYR_SENS_LSB_PER_DPS;
    bias->z = ((float)sum_z / (float)sample_count) / BMI270_GYR_SENS_LSB_PER_DPS;

    printf("[SUCCESS] Calibration Done! Bias -> X:%.1f, Y:%.1f, Z:%.1f dps\r\n",
            bias->x, bias->y, bias->z);

    return HAL_OK;
}

/**
  * @brief  자이로 6바이트 직접 버스트 판독 (FIFO 에는 가속도만 적재하므로 필요 시 호출)
  */
HAL_StatusTypeDef BMI270_ReadGyro(BMI270_Data_t *gyro, const BMI270_Data_t *bias)
{
    if (gyro == NULL) return HAL_ERROR;

    uint8_t tx_buf[8] = { 0 };
    uint8_t rx_buf[8] = { 0 };
    tx_buf[0] = (uint8_t)(BMI270_REG_GYR_X_LSB | 0x80);

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 8, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) return status;

    // 주소 1B + 더미 1B 를 건너뛴 rx_buf[2] 부터가 유효 데이터
    int16_t rx = (int16_t)(((uint16_t)rx_buf[3] << 8) | rx_buf[2]);
    int16_t ry = (int16_t)(((uint16_t)rx_buf[5] << 8) | rx_buf[4]);
    int16_t rz = (int16_t)(((uint16_t)rx_buf[7] << 8) | rx_buf[6]);

    gyro->x = (float)rx / BMI270_GYR_SENS_LSB_PER_DPS;
    gyro->y = (float)ry / BMI270_GYR_SENS_LSB_PER_DPS;
    gyro->z = (float)rz / BMI270_GYR_SENS_LSB_PER_DPS;

    if (bias != NULL)
    {
        gyro->x -= bias->x;
        gyro->y -= bias->y;
        gyro->z -= bias->z;
    }
    return HAL_OK;
}

/**
  * @brief  FIFO(가속도 단독, 헤더리스) 및 INT 핀 라우팅 구성
  *         INT1 <- FIFO watermark / INT2 <- any-motion
  */
HAL_StatusTypeDef BMI270_ConfigFifoAndInterrupts(void)
{
    /* fifo_time_en 을 반드시 꺼둔다. 켜져 있으면 블록 끝에 sensortime 프레임이
     * 덧붙어 고정 6바이트 스트라이드 해석이 통째로 어긋난다. */
    if (BMI270_WriteRegister(BMI270_REG_FIFO_CONFIG_0, 0x00) != HAL_OK) return HAL_ERROR;

    /* 헤더 비활성 + 가속도만 적재 → 프레임당 정확히 6바이트 */
    if (BMI270_WriteRegister(BMI270_REG_FIFO_CONFIG_1, BMI270_FIFO_ACC_EN) != HAL_OK) return HAL_ERROR;

    /* 필터링된 가속도 데이터 사용, 다운샘플링 없음 */
    if (BMI270_WriteRegister(BMI270_REG_FIFO_DOWNS, 0x80) != HAL_OK) return HAL_ERROR;

    /* 워터마크 설정 (바이트 단위) */
    if (BMI270_WriteRegister(BMI270_REG_FIFO_WTM_0, (uint8_t)(BMI270_FIFO_WTM_BYTES & 0xFF)) != HAL_OK) return HAL_ERROR;
    if (BMI270_WriteRegister(BMI270_REG_FIFO_WTM_1, (uint8_t)((BMI270_FIFO_WTM_BYTES >> 8) & 0x1F)) != HAL_OK) return HAL_ERROR;

    /* INT 핀 전기적 특성: 푸시풀 / 액티브 하이 → STM32 EXTI 상승엣지와 일치 */
    if (BMI270_WriteRegister(BMI270_REG_INT1_IO_CTRL, BMI270_INT_IO_PUSHPULL_AH) != HAL_OK) return HAL_ERROR;
    if (BMI270_WriteRegister(BMI270_REG_INT2_IO_CTRL, BMI270_INT_IO_PUSHPULL_AH) != HAL_OK) return HAL_ERROR;

    /* 인터럽트 소스 라우팅 */
    if (BMI270_WriteRegister(BMI270_REG_INT_MAP_DATA,  BMI270_INT1_FWM) != HAL_OK) return HAL_ERROR;
    if (BMI270_WriteRegister(BMI270_REG_INT1_MAP_FEAT, 0x00) != HAL_OK) return HAL_ERROR;
    if (BMI270_WriteRegister(BMI270_REG_INT2_MAP_FEAT, BMI270_INT_ANY_MOT) != HAL_OK) return HAL_ERROR;

    return BMI270_FifoFlush();
}

/**
  * @brief  피처 페이지 16바이트(0x30~0x3F) 통째 판독
  */
static HAL_StatusTypeDef bmi270_feature_page_read(uint8_t page, uint8_t *buf16)
{
    if (BMI270_WriteRegister(BMI270_REG_FEAT_PAGE, page) != HAL_OK) return HAL_ERROR;

    uint8_t tx[18] = {0};
    uint8_t rx[18] = {0};
    tx[0] = (uint8_t)(BMI270_REG_FEATURES | 0x80);

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 18, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (st == HAL_OK) memcpy(buf16, &rx[2], 16);   // 주소 1B + 더미 1B 건너뜀
    return st;
}

/**
  * @brief  피처 페이지 16바이트 통째 기록 (CS 를 끝까지 물고 한 번에 밀어넣는다)
  */
static HAL_StatusTypeDef bmi270_feature_page_write(uint8_t page, const uint8_t *buf16)
{
    if (BMI270_WriteRegister(BMI270_REG_FEAT_PAGE, page) != HAL_OK) return HAL_ERROR;

    uint8_t tx[17];
    tx[0] = (uint8_t)(BMI270_REG_FEATURES & 0x7F);
    memcpy(&tx[1], buf16, 16);

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi2, tx, 17, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    return st;
}

/**
  * @brief  any-motion 피처 구성 (웨이크업 게이트 전용)
  *
  * 피처 엔진 페이지 1, 오프셋 0x0C → 레지스터 0x3C~0x3F 에 16비트 워드 2개가 놓인다.
  *   anymo_1 (0x3C/0x3D) : bit[12:0] duration(20ms 단위) | x_sel<<13 | y_sel<<14 | z_sel<<15
  *   anymo_2 (0x3E/0x3F) : bit[10:0] threshold(5.11 형식, 1/2048 g 단위) | enable<<15
  *
  * @note  임계값은 하드웨어상 최대 약 1g 이다. 이 기능은 "완전 정지 시 웨이크업을
  *        생략"하는 절전 게이트일 뿐, 충격 판별 자체는 FIFO 블록 소프트웨어가 담당한다.
  *        따라서 이 구성이 실패해도 낙상 감지 정확도에는 영향이 없다.
  * @retval HAL_ERROR 는 통신 실패 또는 기록값 검증 불일치를 뜻한다.
  */
HAL_StatusTypeDef BMI270_ConfigAnyMotion(uint16_t threshold_mg, uint16_t duration_ms)
{
    /* mg → 5.11 형식(1/2048 g). 11비트를 넘지 않도록 포화시킨다. */
    uint32_t thr = ((uint32_t)threshold_mg * 2048U) / 1000U;
    if (thr > 0x7FFU) thr = 0x7FFU;

    /* ms → 20ms 단위(50Hz 샘플). 13비트 포화. */
    uint32_t dur = (uint32_t)duration_ms / 20U;
    if (dur > 0x1FFFU) dur = 0x1FFFU;

    uint16_t anymo_1 = (uint16_t)(dur & 0x1FFFU) | (1U << 13) | (1U << 14) | (1U << 15); // x/y/z 전축 감시
    uint16_t anymo_2 = (uint16_t)(thr & 0x07FFU) | (1U << 15);                           // enable

    /* 피처 페이지는 반드시 16바이트 통째로 버스트 기록해야 한다.
     * 바이트 단위 개별 쓰기는 상위 바이트만 남고 하위 바이트가 0으로 날아간다. */
    uint8_t page[16] = {0};
    if (bmi270_feature_page_read(0x01, page) != HAL_OK) return HAL_ERROR;

    const uint8_t off = 0x0C;   // 레지스터 0x3C 에 해당하는 페이지 내 오프셋
    page[off + 0] = (uint8_t)(anymo_1 & 0xFF);
    page[off + 1] = (uint8_t)(anymo_1 >> 8);
    page[off + 2] = (uint8_t)(anymo_2 & 0xFF);
    page[off + 3] = (uint8_t)(anymo_2 >> 8);

    if (bmi270_feature_page_write(0x01, page) != HAL_OK) return HAL_ERROR;

    /* 기록값 검증 — 피처 엔진 오프셋이 다른 config 변종을 물었을 때를 잡아낸다. */
    uint8_t rb_page[16] = {0};
    if (bmi270_feature_page_read(0x01, rb_page) != HAL_OK) return HAL_ERROR;

    uint16_t rb1 = (uint16_t)rb_page[off + 0] | ((uint16_t)rb_page[off + 1] << 8);
    uint16_t rb2 = (uint16_t)rb_page[off + 2] | ((uint16_t)rb_page[off + 3] << 8);

    /* 페이지를 0으로 되돌린다. 열어둔 채로 두면 이후 0x30~0x3F 접근이
     * 엉뚱한 피처 영역을 가리키게 된다. */
    (void)BMI270_WriteRegister(BMI270_REG_FEAT_PAGE, 0x00);

    if (rb1 != anymo_1 || rb2 != anymo_2)
    {
        printf("[ WARN ] any-motion 검증 실패 (w:%04X/%04X r:%04X/%04X) — 절전 게이트만 비활성, 감지는 정상\r\n",
               anymo_1, anymo_2, rb1, rb2);
        return HAL_ERROR;
    }

    printf("[ SUCCESS ] any-motion 게이트 구성 완료 (%umg / %ums)\r\n",
           (unsigned)threshold_mg, (unsigned)duration_ms);
    return HAL_OK;
}

/**
  * @brief  FIFO 강제 비우기
  */
HAL_StatusTypeDef BMI270_FifoFlush(void)
{
    return BMI270_WriteRegister(BMI270_REG_CMD, BMI270_CMD_FIFO_FLUSH);
}

/**
  * @brief  현재 FIFO 에 적재된 바이트 수 조회 (13비트 유효)
  */
HAL_StatusTypeDef BMI270_GetFifoLength(uint16_t *bytes)
{
    if (bytes == NULL) return HAL_ERROR;

    uint8_t tx_buf[4] = { (uint8_t)(BMI270_REG_FIFO_LENGTH_0 | 0x80), 0, 0, 0 };
    uint8_t rx_buf[4] = { 0 };

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 4, 100);
    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) return status;

    *bytes = (uint16_t)(((uint16_t)(rx_buf[3] & 0x1F) << 8) | rx_buf[2]);
    return HAL_OK;
}

/**
  * @brief  FIFO 블록을 SPI DMA 로 일괄 수거 시작 (CS 해제는 완료 콜백이 담당)
  * @param  rx_buf     최소 data_bytes + BMI270_SPI_HDR_LEN 크기의 버퍼
  * @param  data_bytes 실제로 받아낼 FIFO 데이터 바이트 수
  */
HAL_StatusTypeDef BMI270_StartFifoRead_DMA(uint8_t *rx_buf, uint16_t data_bytes)
{
    static uint8_t s_fifo_tx_buf[BMI270_FIFO_RX_LEN] = { 0 };

    if (rx_buf == NULL || data_bytes == 0) return HAL_ERROR;
    if (data_bytes > BMI270_FIFO_RX_LEN - BMI270_SPI_HDR_LEN) return HAL_ERROR;

    uint16_t total = data_bytes + BMI270_SPI_HDR_LEN;

    s_fifo_tx_buf[0] = (uint8_t)(BMI270_REG_FIFO_DATA | 0x80);
    // 나머지는 0 유지 — 정적 버퍼라 매번 memset 할 필요가 없다.

    HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive_DMA(&hspi2, s_fifo_tx_buf, rx_buf, total);

    if (status != HAL_OK)
    {
        HAL_GPIO_WritePin(BMI270_CS_GPIO_Port, BMI270_CS_Pin, GPIO_PIN_SET);
    }
    return status;
}

/**
  * @brief  수거한 FIFO 블록을 가속도 배열(g 단위)로 디코딩
  * @return 실제로 해석된 유효 프레임 수
  *
  * 헤더리스 + 가속도 단독이므로 프레임은 고정 6바이트다.
  * FIFO 가 비면 0x8000(=-32768) 패턴이 채워지므로 이를 만나면 즉시 종료한다.
  */
uint16_t BMI270_ParseFifoBlock(const uint8_t *rx_buf, uint16_t data_bytes,
                               BMI270_Data_t *out, uint16_t max_frames)
{
    if (rx_buf == NULL || out == NULL) return 0;

    const uint8_t *p = rx_buf + BMI270_SPI_HDR_LEN;  // 주소 1B + 더미 1B 건너뜀
    uint16_t frames = data_bytes / BMI270_FIFO_FRAME_LEN;
    if (frames > max_frames) frames = max_frames;

    uint16_t count = 0;
    for (uint16_t i = 0; i < frames; i++)
    {
        const uint8_t *f = p + (i * BMI270_FIFO_FRAME_LEN);

        int16_t rx_ = (int16_t)(((uint16_t)f[1] << 8) | f[0]);
        int16_t ry_ = (int16_t)(((uint16_t)f[3] << 8) | f[2]);
        int16_t rz_ = (int16_t)(((uint16_t)f[5] << 8) | f[4]);

        // FIFO 언더런 마커 — 이후는 전부 무효 데이터다.
        if (rx_ == (int16_t)0x8000 && ry_ == (int16_t)0x8000 && rz_ == (int16_t)0x8000)
        {
            break;
        }

        out[count].x = (float)rx_ / BMI270_ACC_SENS_LSB_PER_G;
        out[count].y = (float)ry_ / BMI270_ACC_SENS_LSB_PER_G;
        out[count].z = (float)rz_ / BMI270_ACC_SENS_LSB_PER_G;
        count++;
    }

    return count;
}
