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
#define DBJ_EVT_EGRESS      0x02    /* 침상 이탈 확정 (x,y = 발생 위치) */

/* 제어 메시지 타입 (dbj_ctrl_header_t.type) */
#define DBJ_CTRL_ROI_SET    0x01    /* 채널 ROI 설정 — 헤더 뒤에 점 배열이 옴 */
#define DBJ_CTRL_ROI_CLEAR  0x02    /* 채널 ROI 삭제 — 점 배열 없음 */
#define DBJ_CTRL_ALARM_CONFIRM 0x03  /* 통합 경보 해제 — 점 배열 없음 */
#define DBJ_CTRL_RISK_UPDATE  0x04  /* 입소자 위험도 갱신*/
/* 채널 카메라 런타임 연결/해제 — Qt에서 CCTV를 지정하면 서버가 그 RTSP를 연다.
 * cameras.conf에 URL을 미리 박지 않고, 관제 화면에서 카메라를 추가하는 구조.
 *  - CAMERA_SET  : reserved(uint16)=이어지는 RTSP URL 문자열의 바이트 길이.
 *                  헤더 뒤에 그만큼의 URL 바이트가 온다(널종단 없음). 서버는 해당
 *                  채널 RtspAvClient를 이 URL로 (재)연결한다. 카메라 1대(4센서)의
 *                  채널별 서브스트림 URL은 Qt가 만들어 채널마다 따로 보낸다.
 *  - CAMERA_CLEAR: 문자열 없음. 해당 채널 연결을 끊고 대기 상태로 되돌린다. */
#define DBJ_CTRL_CAMERA_SET   0x05  /* 채널 카메라 연결 — 헤더 뒤에 RTSP URL 문자열 */
#define DBJ_CTRL_CAMERA_CLEAR 0x06  /* 채널 카메라 해제 — 문자열 없음 */
/* 카메라 이미지 파라미터 조절 — 헤더 뒤에 dbj_image_params_t 1개가 온다.
 * 서버는 해당 채널 카메라에 ONVIF Imaging(SetImagingSettings)으로 밝기/대비/채도를
 * 적용한다. 값은 0~100 정규화 — 서버가
 * 카메라 모델별 실제 범위로 매핑한다(클라는 카메라 스펙을 몰라도 된다). */
#define DBJ_CTRL_IMAGE_SET    0x07  /* 채널 카메라 이미지 파라미터 — 헤더 뒤 dbj_image_params_t */

#define DBJ_CAMERA_URL_MAX  512     /* CAMERA_SET URL 문자열 길이 상한 */

/* 위험도 값 정의 (3단계 분기) */
#define DBJ_RISK_LOW          1       /* 위험도 '하' -> 이탈해도 상관없음 (패스) */
#define DBJ_RISK_MID          2       /* 위험도 '중' -> 특정 시간대에만 경보 */
#define DBJ_RISK_HIGH         3       /* 위험도 '상' -> 언제든 이탈하면 즉시 경보 */

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
    uint8_t  point_count;  /* ROI_SET 시 점 개수(0~32) 또는 RISK_UPDATE 시 위험도 값(1~3) */
    uint16_t reserved;     /* 기본 0. CAMERA_SET 시엔 이어지는 URL 문자열 바이트 길이 */
} dbj_ctrl_header_t;       /* 8바이트, 이어서 point_count개의 dbj_roi_point_t 또는 reserved 바이트의 URL */

typedef struct {
    uint16_t x;            /* 정규화 x × DBJ_ROI_COORD_SCALE (0~10000) */
    uint16_t y;            /* 정규화 y × DBJ_ROI_COORD_SCALE (0~10000) */
} dbj_roi_point_t;         /* 4바이트 */

typedef struct {
    uint8_t brightness;    /* 0~100 (서버가 카메라 실제 범위로 매핑) */
    uint8_t contrast;      /* 0~100 */
    uint8_t saturation;    /* 0~100 */
} dbj_image_params_t;      /* 3바이트, IMAGE_SET 시 헤더 뒤에 1개 */

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
