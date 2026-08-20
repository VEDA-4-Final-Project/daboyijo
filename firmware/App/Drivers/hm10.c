/*
 * hm10.c — HM-10 BLE 송수신 모듈 (구현)
 *
 * 투과모드라 STM32는 UART로 7바이트 패킷만 보내면 HM-10이 BLE로 넘겨줌.
 * 패킷 구조는 이 파일 안에만 있음 → 나중에 바뀌면 여기 hm10_send_packet만 수정.
 * (단, 수신 측 relay-node 도 함께 고쳐야 한다 — hm10.h 의 경고 참조)
 *
 * 반대 방향도 같은 UART 를 쓴다. RPi 가 FFE1 에 write 하면 HM-10 이 UART TX 로
 * 흘려보내고 STM32 의 USART2 RX 로 들어온다. 지금 내려오는 것은 시각 하나뿐이며
 * (AppClock 씨앗), 이 파일 아래쪽 수신부가 담당한다.
 */
#include "hm10.h"

/* ------------------------------------------------------------------ */
static UART_HandleTypeDef *s_huart = NULL;   /* HM-10 연결 UART */
#define HM10_TX_TIMEOUT  100                 /* 블로킹 전송 타임아웃 (ms) */

/* 값 범위 클램프 (0~max) */
static inline uint8_t hm10_clamp(uint32_t v, uint8_t max)
{
    return (v > max) ? max : (uint8_t)v;
}

/* ------------------------------------------------------------------ */
void hm10_init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
}

void hm10_send_packet(uint8_t hr, uint8_t spo2, uint8_t fall_flag, uint16_t steps)
{
    if (s_huart == NULL) return;             /* init 안 했으면 무시 */

    uint8_t pkt[HM10_PKT_LEN];
    pkt[0] = HM10_PKT_HEADER;                /* 0xAA */
    pkt[1] = hr;                             /* 0~255 그대로 (클램프 없음) */
    pkt[2] = hm10_clamp(spo2, 100);          /* 0~100 클램프 */
    pkt[3] = (fall_flag != HM10_FALL_NONE) ? HM10_FALL_SUSPECT : HM10_FALL_NONE;
    pkt[4] = (uint8_t)(steps & 0xFFU);
    pkt[5] = (uint8_t)((steps >> 8) & 0xFFU);

    /* 체크섬 = 앞선 모든 바이트의 XOR (헤더 포함).
     * 길이 상수로 도는 루프라 필드를 더하거나 빼도 여기는 고칠 필요가 없다. */
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < HM10_PKT_LEN - 1; i++)
    {
        checksum ^= pkt[i];
    }
    pkt[HM10_PKT_LEN - 1] = checksum;

    HAL_UART_Transmit(s_huart, pkt, HM10_PKT_LEN, HM10_TX_TIMEOUT);
}

/* ==================================================================
 * 수신 — RPi 가 BLE 로 내려주는 시각 패킷
 * ================================================================== */

static uint8_t          s_rx_byte;                      /* HAL 이 채워주는 1바이트 창구 */
static uint8_t          s_rx_buf[HM10_RX_PKT_LEN];      /* 조립 중인 패킷 */
static uint8_t          s_rx_len = 0;                   /* 지금까지 모인 바이트 수 */

static volatile uint8_t s_time_pending = 0;             /* 검증까지 끝난 시각이 대기 중 */
static volatile uint8_t s_time_h = 0, s_time_m = 0, s_time_s = 0;

/* 수신 계측 (hm10.h 의 hm10_get_rx_stats 주석 참조).
 * ISR 이 쓰고 메인 루프가 읽는다. 32비트 정렬 변수라 Cortex-M 에서 판독은
 * 원자적이고, 통계 용도라 한 틱 늦게 보여도 상관없다 — 잠금을 두지 않는다. */
static volatile uint32_t s_rx_total = 0;                /* UART 로 들어온 총 바이트 */
static volatile uint32_t s_rx_errors = 0;               /* ORE/FE/NE 등 오류 횟수 */
static volatile uint32_t s_rx_bad = 0;                  /* 검증에서 버려진 패킷 수 */
static volatile uint8_t  s_rx_last = 0;                 /* 마지막으로 받은 바이트 */

HAL_StatusTypeDef hm10_start_receive(void)
{
    if (s_huart == NULL) return HAL_ERROR;
    s_rx_len = 0;
    /* 결과를 삼키지 않고 돌려준다 — 무장에 실패하면 수신이 통째로 죽는데,
     * 겉으로는 '시계가 안 맞는다' 로만 보여 원인을 찾기 어렵다. */
    return HAL_UART_Receive_IT(s_huart, &s_rx_byte, 1);
}

void hm10_request_time(void)
{
    if (s_huart == NULL) return;

    uint8_t pkt[HM10_REQ_LEN];
    pkt[0] = HM10_REQ_HEADER;
    pkt[1] = HM10_REQ_TIME;
    pkt[2] = (uint8_t)(pkt[0] ^ pkt[1]);

    HAL_UART_Transmit(s_huart, pkt, HM10_REQ_LEN, HM10_TX_TIMEOUT);
}

/**
  * @brief  1바이트 수신 완료 ISR.
  *
  * 헤더(0x55)를 만나기 전 바이트는 버리고, 헤더부터 5바이트를 모아 검증한다.
  * 검증에 실패하면 조립을 처음부터 다시 한다 — BLE 가 링크 계층에서 이미
  * 무결성을 보장하므로 여기까지 깨져 올 일은 드물고, 실패해도 RPi 가 주기적으로
  * 다시 보내주므로 정교한 재동기가 필요 없다.
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (s_huart == NULL || huart->Instance != s_huart->Instance) return;

    /* 검증 이전에 먼저 센다. 헤더가 아니어서 버릴 바이트도 '선이 살아있다' 는
     * 증거이므로, 걸러내기 전에 세어야 진단에 쓸모가 있다. */
    s_rx_total++;
    s_rx_last = s_rx_byte;

    if (s_rx_len == 0)
    {
        /* 헤더를 기다리는 중 — 아닌 바이트는 그냥 버린다 */
        if (s_rx_byte == HM10_RX_HEADER)
        {
            s_rx_buf[s_rx_len++] = s_rx_byte;
        }
    }
    else
    {
        s_rx_buf[s_rx_len++] = s_rx_byte;

        if (s_rx_len >= HM10_RX_PKT_LEN)
        {
            uint8_t checksum = 0;
            for (uint8_t i = 0; i < HM10_RX_PKT_LEN - 1; i++)
            {
                checksum ^= s_rx_buf[i];
            }

            /* 체크섬과 값 범위를 모두 통과해야 받아들인다.
             * 범위 검사는 헤더를 잘못 잡았을 때의 2차 방어선이다. */
            if (checksum == s_rx_buf[HM10_RX_PKT_LEN - 1] &&
                s_rx_buf[1] <= 23 && s_rx_buf[2] <= 59 && s_rx_buf[3] <= 59)
            {
                s_time_h = s_rx_buf[1];
                s_time_m = s_rx_buf[2];
                s_time_s = s_rx_buf[3];
                s_time_pending = 1;
            }
            else
            {
                s_rx_bad++;   /* 5바이트는 왔는데 규격이 안 맞는다 — 양쪽 스펙을 의심할 것 */
            }
            s_rx_len = 0;
        }
    }

    HAL_UART_Receive_IT(s_huart, &s_rx_byte, 1);   /* 재무장 — 빠뜨리면 첫 바이트 이후 영영 안 받는다 */
}

/**
  * @brief  UART 오류 콜백 — 오버런 등으로 수신이 멎으면 되살린다.
  *
  * 1바이트 IT 수신은 무장되지 않은 사이에 바이트가 들어오면 ORE 가 서고 HAL 이
  * 수신을 중단시킨다. 여기서 다시 무장하지 않으면 그 순간부터 시각 동기가
  * 영구히 죽는데, 겉으로는 '시계가 안 맞는다' 로만 보여 원인을 찾기 어렵다.
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (s_huart == NULL || huart->Instance != s_huart->Instance) return;

    s_rx_errors++;
    s_rx_len = 0;
    HAL_UART_Receive_IT(s_huart, &s_rx_byte, 1);
}

void hm10_get_rx_stats(uint32_t *total, uint32_t *errors, uint32_t *bad, uint8_t *last)
{
    if (total)  *total  = s_rx_total;
    if (errors) *errors = s_rx_errors;
    if (bad)    *bad    = s_rx_bad;
    if (last)   *last   = s_rx_last;
}

uint8_t hm10_take_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!s_time_pending) return 0;

    /* ISR 이 세 값을 갱신하는 도중에 끼어들면 시/분/초가 서로 다른 패킷에서
     * 섞여 나올 수 있다. 세 바이트 복사라 인터럽트를 잠깐 막는 비용이 없다. */
    __disable_irq();
    if (hour)   *hour   = s_time_h;
    if (minute) *minute = s_time_m;
    if (second) *second = s_time_s;
    s_time_pending = 0;
    __enable_irq();

    return 1;
}
