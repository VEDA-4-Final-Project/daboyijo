/**
 * @file    protocol.h
 * @brief   다보이조 공통 통신 프로토콜 정의 (전 모듈 공유)
 *
 * 사용 구간:
 *  - STM32(웨어러블) → RPi(중계 노드) : UART 바이너리 프레임
 *  - 중계 노드 → 중앙 서버           : Wi-Fi/TCP (동일 프레임 캡슐화)
 *
 * ⚠️ 이 파일을 변경할 때는 반드시 전 모듈 담당자와 합의 후 PR로 반영할 것.
 *    (구조체 변경 = 전 모듈 재빌드 필요)
 *
 * C(STM32 펌웨어)와 C++(서버/노드) 양쪽에서 include 가능.
 */

#ifndef DBJ_PROTOCOL_H
#define DBJ_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 프레임 공통 ============================ */

#define DBJ_MAGIC           0xDB4A  /* 프레임 시작 식별자 */
#define DBJ_PROTO_VERSION   0x01

/* 프레임 구조:
 * [dbj_header_t][payload (payload_len 바이트)][crc16 (2바이트, 리틀엔디언)]
 * CRC16-CCITT(0xFFFF 초기값), header 첫 바이트부터 payload 끝까지 계산
 */

/* ============================ 메시지 타입 ============================ */

typedef enum {
    DBJ_MSG_HEARTBEAT   = 0x00, /* 생존 신호 (주기 송신) */
    DBJ_MSG_FALL_EVENT  = 0x01, /* IMU 낙상 충격 감지 이벤트 */
    DBJ_MSG_BIO_DATA    = 0x02, /* 심박/체온 주기 데이터 */
    DBJ_MSG_WEAR_STATE  = 0x03, /* 착용/미착용 상태 변화 */
    DBJ_MSG_BATTERY_LOW = 0x04, /* 배터리 부족 경고 */
    DBJ_MSG_ACK         = 0x10, /* 수신 확인 */
    DBJ_MSG_NACK        = 0x11  /* 수신 실패 (재전송 요청) */
} dbj_msg_type_t;

/* ============================ 헤더 ============================ */

#pragma pack(push, 1)

typedef struct {
    uint16_t magic;        /* DBJ_MAGIC */
    uint8_t  version;      /* DBJ_PROTO_VERSION */
    uint8_t  device_id;    /* 웨어러블 기기 ID (= 입소자 매핑 키) */
    uint8_t  msg_type;     /* dbj_msg_type_t */
    uint8_t  seq;          /* 시퀀스 번호 (유실 감지용, 0~255 순환) */
    uint8_t  payload_len;  /* payload 바이트 수 */
} dbj_header_t;

/* ============================ 페이로드 ============================ */

/* DBJ_MSG_FALL_EVENT */
typedef struct {
    uint32_t timestamp;    /* Unix time (초). STM RTC 미사용 시 0, 중계노드가 채움 */
    int16_t  acc_x_mg;     /* 충격 시점 가속도 (mg) */
    int16_t  acc_y_mg;
    int16_t  acc_z_mg;
    uint8_t  impact_level; /* 충격 강도 등급 1~5 */
} dbj_fall_event_t;

/* DBJ_MSG_BIO_DATA */
typedef struct {
    uint32_t timestamp;
    uint8_t  heart_rate_bpm;   /* 0 = 측정 불가 */
    uint16_t skin_temp_x100;   /* 36.5°C → 3650 */
    uint8_t  worn;             /* 1 착용 / 0 미착용 (PPG·온도 기반 판정) */
    uint8_t  battery_pct;      /* 0~100 */
} dbj_bio_data_t;

/* DBJ_MSG_WEAR_STATE */
typedef struct {
    uint32_t timestamp;
    uint8_t  worn;             /* 1 착용 / 0 미착용 */
} dbj_wear_state_t;

#pragma pack(pop)

/* ============================ MQTT 토픽 ============================ */
/* 서버 ↔ 알림 노드 / 클라이언트 간 이벤트 전파 */

#define DBJ_TOPIC_ALERT_CMD     "dbj/alert/%d/cmd"     /* 서버→알림노드: 알림 발생/해제 */
#define DBJ_TOPIC_ALERT_STATUS  "dbj/alert/%d/status"  /* 알림노드→서버: 상태 보고 */
#define DBJ_TOPIC_EVENT_FALL    "dbj/event/fall"       /* 서버→구독자: 낙상 확정 이벤트 */

/* 알림 명령 (cmd 페이로드, JSON: {"cmd": n, "channel": n}) */
typedef enum {
    DBJ_ALERT_OFF        = 0,  /* 알림 해제 */
    DBJ_ALERT_FALL       = 1,  /* 낙상 확정 — 사이렌+LED */
    DBJ_ALERT_SUSPECT    = 2   /* 낙상 의심(페일세이프) — 차상위 경보 */
} dbj_alert_cmd_t;

#ifdef __cplusplus
}
#endif

#endif /* DBJ_PROTOCOL_H */
