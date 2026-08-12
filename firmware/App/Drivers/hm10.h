/*
 * hm10.h — HM-10 BLE 송신 모듈 (인터페이스)
 *
 * STM32(웨어러블) → HM-10 → BLE → 중계 노드(RPi)
 * 투과모드라 UART 로 바이트만 밀어내면 그대로 BLE 로 넘어감
 *
 * 사용법: UART 초기화 후 hm10_init(&huart2) 1회 → hm10_send_packet(...)
 *
 * ── BLE 패킷 구조 (5바이트, 팀 공용 스펙) ──────────────────────
 *   [0] 0xAA           패킷 시작 헤더 (프레이밍 식별자)
 *   [1] heart_rate     심박수 (bpm)      0~255
 *   [2] spo2           산소포화도 (%)    0~100
 *   [3] temp           온도 (정수 °C)    0~100
 *   [4] fall_flag      낙상 의심 플래그  0x00 정상 / 0x01 의심
 * ────────────────────────────────────────────────────────────
 */
#ifndef HM10_H
#define HM10_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 패킷 상수 */
#define HM10_PKT_HEADER   0xAA
#define HM10_PKT_LEN      5

/* 낙상 의심 플래그 값 */
#define HM10_FALL_NONE    0x00
#define HM10_FALL_SUSPECT 0x01

/* HM-10 이 연결된 UART 핸들 등록 (부팅 시 1회) */
void hm10_init(UART_HandleTypeDef *huart);

/* 5바이트 패킷 송신 — 전송 예: AA 4E 62 25 01 (78bpm, 98%, 37°C, 낙상의심)
 *   hr        : 심박수 bpm      (클램프 없음, 0~255 그대로)
 *   spo2      : 산소포화도 %    (0~100 클램프)
 *   temp      : 온도 정수 °C    (0~100 클램프)
 *   fall_flag : 낙상 의심 0/1   (0 이외는 1 로 정규화) */
void hm10_send_packet(uint8_t hr, uint8_t spo2, uint8_t temp, uint8_t fall_flag);

#endif /* HM10_H */
