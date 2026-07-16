/**
 * @file    video_stream.h
 * @brief   중앙 서버 → Qt 관제 클라이언트 영상 스트림 패킷 정의
 *
 * 사용 구간:
 *  - 중앙 서버(RPi 4) → Qt 관제 클라이언트 : TCP (v1 평문, 추후 OpenSSL TLS 적용)
 *
 * 스트림 구조: [dbj_vs_header_t][JPEG payload_len 바이트] 의 반복
 *  - 모든 다바이트 필드는 리틀엔디언
 *  - TCP가 전송 무결성을 보장하므로 CRC 없음 (UART 구간의 protocol.h와 다른 점)
 *
 * 역방향(클라 → 서버) 제어 채널:
 *  - 같은 TCP 연결로 Qt가 침대 ROI 다각형을 서버에 보낸다.
 *  - 구조: [dbj_ctrl_header_t][dbj_roi_point_t × point_count]
 *  - magic이 DBJ_CTRL_MAGIC(0xDB4C)라 영상 프레임과 구분된다.
 *
 * Qt 수신 절차:
 *  1. 헤더 16바이트 수신 → magic/version 검증 (안 맞으면 연결 재수립)
 *  2. payload_len 바이트 수신
 *  3. QImage::fromData(payload, "JPEG") 로 디코딩 후 채널별 위젯에 표시
 *
 * ⚠️ 이 파일을 변경할 때는 서버·클라이언트 담당자와 합의 후 PR로 반영할 것.
 */

#ifndef DBJ_VIDEO_STREAM_H
#define DBJ_VIDEO_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBJ_VS_MAGIC        0xDB4B  /* 영상 프레임 시작 식별자 */
#define DBJ_VS_VERSION      0x01
#define DBJ_VS_PORT_DEFAULT 5500    /* 서버 기본 송출 포트 */

/* ── 클라이언트 → 서버 제어 메시지 (역방향) ──────────────────
 * 같은 TCP 연결의 반대 방향으로 흐른다 (영상은 서버→클라, 제어는 클라→서버).
 * 현재 용도: Qt 관제 화면에서 그린 침대 ROI 다각형을 서버로 전달.
 * 좌표는 화면 대비 0~1 정규화값을 10000배한 고정소수(uint16) — 서버의
 * Detection.cx/cy(0~1)와 같은 좌표계라 그대로 점-다각형 판정에 쓸 수 있다.
 */
#define DBJ_CTRL_MAGIC      0xDB4C  /* 제어 메시지 시작 식별자 (영상과 구분) */

/* ── 서버 → 클라이언트 이벤트 메시지 ─────────────────────────
 * 영상 프레임과 같은 TCP 스트림에 끼어 내려온다(순서 보장).
 * Qt는 magic으로 영상(0xDB4B)/이벤트(0xDB4D)를 구분해 파싱한다.
 * 현재 용도: 낙상 확정 통보 → 해당 채널 강조 + 팝업.
 */
#define DBJ_EVT_MAGIC       0xDB4D  /* 이벤트 메시지 시작 식별자 */
#define DBJ_EVT_FALL        0x01    /* 낙상 확정 (x,y = 발생 위치) */
#define DBJ_EVT_ESCAPE      0x02    /* 침상 이탈 (x,y = 발생 위치) */

/* 제어 메시지 타입 (dbj_ctrl_header_t.type) */
#define DBJ_CTRL_ROI_SET    0x01    /* 채널 ROI 설정 — 헤더 뒤에 점 배열이 옴 */
#define DBJ_CTRL_ROI_CLEAR  0x02    /* 채널 ROI 삭제 — 점 배열 없음 */
#define DBJ_CTRL_FALL_CONFIRM 0x03  /* 낙상 경보 해제 — 점 배열 없음 */

#define DBJ_ROI_MAX_POINTS  32      /* 다각형 꼭짓점 상한 */
#define DBJ_ROI_COORD_SCALE 10000   /* 정규화 좌표 고정소수 배율 (0.0~1.0 → 0~10000) */

#pragma pack(push, 1)

typedef struct {
    uint16_t magic;        /* DBJ_VS_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  channel;      /* CCTV 채널 0~3 */
    uint64_t timestamp_ms; /* 서버 Unix time (밀리초) — 클라이언트 지연 측정용 */
    uint32_t payload_len;  /* 이어지는 JPEG 데이터 바이트 수 */
} dbj_vs_header_t;         /* 16바이트 */

typedef struct {
    uint16_t magic;        /* DBJ_CTRL_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  type;         /* DBJ_CTRL_* */
    uint8_t  channel;      /* 대상 채널 0~3 */
    uint8_t  point_count;  /* 이어지는 점 개수 (ROI_SET 전용, 0~DBJ_ROI_MAX_POINTS) */
    uint16_t reserved;     /* 4바이트 정렬용 (0으로) */
} dbj_ctrl_header_t;       /* 8바이트, 이어서 point_count개의 dbj_roi_point_t */

typedef struct {
    uint16_t x;            /* 정규화 x × DBJ_ROI_COORD_SCALE (0~10000) */
    uint16_t y;            /* 정규화 y × DBJ_ROI_COORD_SCALE (0~10000) */
} dbj_roi_point_t;         /* 4바이트 */

typedef struct {
    uint16_t magic;        /* DBJ_EVT_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  type;         /* DBJ_EVT_* */
    uint8_t  channel;      /* 발생 채널 0~3 */
    uint8_t  reserved;     /* 0 */
    uint16_t x;            /* 발생 위치 정규화 x × DBJ_ROI_COORD_SCALE (없으면 0) */
    uint16_t y;            /* 발생 위치 정규화 y × DBJ_ROI_COORD_SCALE (없으면 0) */
    uint64_t timestamp_ms; /* 서버 Unix time (밀리초) */
} dbj_evt_header_t;        /* 18바이트, 페이로드 없음 */

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* DBJ_VIDEO_STREAM_H */
