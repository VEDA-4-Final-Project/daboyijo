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

#pragma pack(push, 1)

typedef struct {
    uint16_t magic;        /* DBJ_VS_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  channel;      /* CCTV 채널 0~3 */
    uint64_t timestamp_ms; /* 서버 Unix time (밀리초) — 클라이언트 지연 측정용 */
    uint32_t payload_len;  /* 이어지는 JPEG 데이터 바이트 수 */
} dbj_vs_header_t;         /* 16바이트 */

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* DBJ_VIDEO_STREAM_H */
