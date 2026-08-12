/*
 * hm10.c — HM-10 BLE 송신 모듈 (구현)
 *
 * 투과모드라 UART 로 5바이트만 보내면 HM-10 이 BLE 로 넘겨줌
 * 패킷 조립은 hm10_send_packet 한 곳에만 있어 스펙이 바뀌면 여기만 수정
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

void hm10_send_packet(uint8_t hr, uint8_t spo2, uint8_t temp, uint8_t fall_flag)
{
    if (s_huart == NULL) return;             /* init 안 했으면 무시 */

    uint8_t pkt[HM10_PKT_LEN];
    pkt[0] = HM10_PKT_HEADER;                /* 0xAA */
    pkt[1] = hr;                             /* 0~255 그대로 (클램프 없음) */
    pkt[2] = hm10_clamp(spo2, 100);          /* 0~100 클램프 */
    pkt[3] = hm10_clamp(temp, 100);          /* 0~100 클램프 */
    pkt[4] = (fall_flag != HM10_FALL_NONE) ? HM10_FALL_SUSPECT : HM10_FALL_NONE;

    HAL_UART_Transmit(s_huart, pkt, HM10_PKT_LEN, HM10_TX_TIMEOUT);
}
