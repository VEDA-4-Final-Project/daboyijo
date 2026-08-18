/**
 * @file    video_stream.h
 * @brief   중앙 서버 → Qt 관제 클라이언트 영상 스트림 패킷 정의
 *
 * 사용 구간:
 *  - 중앙 서버(RPi 4) → Qt 관제 클라이언트 : TCP, OpenSSL TLS(선택)
 *    서버(server/src/config.hpp의 stream_cert_path/stream_key_path)에 인증서를
 *    설정하면 TLS, 비우면 평문 — 프레임 포맷 자체는 TLS 유무와 무관(전송계층만
 *    다르다). 인증서 발급은 MQTT/scripts/generate_stream_certs.sh 참고.
 *
 * 스트림 구조: [dbj_vs_header_t][JPEG payload_len 바이트] 의 반복
 *  - 모든 다바이트 필드는 리틀엔디언
 *  - TCP가 전송 무결성을 보장하므로 CRC 없음 (UART 구간의 protocol.h와 다른 점)
 *
 * 역방향(클라 → 서버) 제어 채널:
 *  - 같은 TCP 연결로 Qt가 침대 ROI 다각형을 서버에 보낸다.
 *  - 구조: [dbj_ctrl_header_t][dbj_roi_point_t × point_count]
 *  - magic이 DBJ_CTRL_MAGIC(0xDB4C)라 영상 프레임과 구분된다.
 *  - 한 채널에 침대가 여러 개일 수 있어 ROI도 채널당 여러 개다(roi_id로 구분).
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
 * roi_id 로 "어느 침대(=누구)"인지가 같이 내려온다 — 서버가 추적 객체를 침대에
 * 귀속시키지 못했으면 DBJ_ROI_ID_NONE 이 오고, Qt는 그때 "미상"으로 표시한다.
 */
#define DBJ_EVT_MAGIC       0xDB4D  /* 이벤트 메시지 시작 식별자 */
#define DBJ_EVT_FALL        0x01    /* 낙상 확정 (x,y = 발생 위치) */
#define DBJ_EVT_EGRESS      0x02    /* 침상 이탈 확정 (x,y = 발생 위치) */
#define DBJ_EVT_VITAL_ABNORMAL 0x03 /* 웨어러블 생체데이터 이상 (x,y 미사용, 0) */

/* ── 서버 → 클라이언트 영상검색 결과 ──────────────────────────
 * 영상 프레임·이벤트와 같은 TCP 스트림에 섞여 내려온다. Qt는 magic으로 구분한다.
 * 케어봇(텔레그램)의 "🔍 영상 검색"과 같은 서버 로직(video_search_module)을
 * Qt 관제 화면에서도 쓸 수 있게 하는 통로 — 질의는 DBJ_CTRL_SEARCH_QUERY(클라→서버,
 * 아래 제어 메시지 절 참고)로 보내고, 응답은 이 메시지로 돌아온다.
 * 응답은 요청을 보낸 그 클라이언트에게만 간다(다른 관제 PC로 새지 않음). */
#define DBJ_SEARCH_MAGIC    0xDB4E  /* 영상검색 결과 시작 식별자 */

/* 제어 메시지 타입 (dbj_ctrl_header_t.type) */
#define DBJ_CTRL_ROI_SET    0x01    /* 침대 ROI 설정 — 헤더 뒤에 점 배열이 옴 */
#define DBJ_CTRL_ROI_CLEAR  0x02    /* 침대 ROI 삭제 — 점 배열 없음 */
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
/* 카메라 포커스(초점) — 헤더 뒤에 dbj_focus_t 1개. 서버는 한화 SUNAPI SimpleFocus
 * (image.cgi?msubmenu=focus&action=control&Mode=SimpleFocus[&FocusAreaCoordinate=..])
 * 로 적용한다. mode=AREA면 클릭 중심 정규화 좌표를 카메라 픽셀좌표로 환산해 영역 초점.*/
#define DBJ_CTRL_FOCUS_SET    0x08  /* 채널 카메라 포커스 — 헤더 뒤 dbj_focus_t */
#define DBJ_FOCUS_WHOLE       0     /* 전체 자동초점 (좌표 무시) */
#define DBJ_FOCUS_AREA        1     /* 클릭 지점 영역 초점 */
/* 침대 ROI ↔ 입소자 매핑 — 헤더 뒤에 dbj_roi_bind_t 1개.
 * "이 채널의 N번 침대는 입소자 R의 자리다"를 서버에 알린다. 서버는 이 매핑을
 * roi_zones 테이블에 남기고, 그 침대에서 태어난 추적 객체에 입소자를 귀속시킨다
 * (core/identity_tracker.hpp). resident_id=0 이면 매핑 해제. */
#define DBJ_CTRL_ROI_BIND     0x09  /* 침대 ↔ 입소자 매핑 — 헤더 뒤 dbj_roi_bind_t */
/* 영상검색 자연어 질의 — 헤더 뒤에 reserved 바이트 길이의 UTF-8 질의 문자열이
 * 온다(CAMERA_SET과 같은 재활용 패턴). channel은 검색 범위(그 방만) — 서버는
 * video_search_module로 처리한 뒤 DBJ_SEARCH_MAGIC 메시지로 이 클라이언트에만 회신한다. */
#define DBJ_CTRL_SEARCH_QUERY 0x0A  /* 영상검색 질의 — 헤더 뒤 UTF-8 질의 문자열 */

#define DBJ_CAMERA_URL_MAX  512     /* CAMERA_SET URL 문자열 길이 상한 */
#define DBJ_SEARCH_QUERY_MAX 300    /* SEARCH_QUERY 질의 문자열 길이 상한(UTF-8 바이트) */

/* 위험도 값 정의 (3단계 분기) */
#define DBJ_RISK_LOW          1       /* 위험도 '하' -> 이탈해도 상관없음 (패스) */
#define DBJ_RISK_MID          2       /* 위험도 '중' -> 특정 시간대에만 경보 */
#define DBJ_RISK_HIGH         3       /* 위험도 '상' -> 언제든 이탈하면 즉시 경보 */

#define DBJ_ROI_MAX_POINTS  32      /* 다각형 꼭짓점 상한 */
#define DBJ_ROI_COORD_SCALE 10000   /* 정규화 좌표 고정소수 배율 (0.0~1.0 → 0~10000) */

/* ── 다중 침대 ROI (채널당 여러 개) ────────────────────────────
 * 한 채널(=한 병실 시야)에 침대가 2개 이상이고, 침대마다 입소자가 다르다.
 * 그래서 ROI는 채널당 1개가 아니라 roi_id로 구분되는 여러 개다.
 *
 * roi_id 는 dbj_ctrl_header_t.reserved 의 하위 8비트로 실어 보낸다
 * (ROI_SET / ROI_CLEAR / RISK_UPDATE). 상위 8비트는 0.
 * ★ 헤더 크기를 안 늘리려고 기존 reserved 를 재해석한 것이라, 예전 클라이언트가
 *   보내던 reserved=0 은 자연히 roi_id=0("첫 번째 침대")이 되어 하위호환된다.
 *   CAMERA_SET 만은 예전대로 reserved 전체가 URL 길이다(ROI 계열이 아님).
 */
#define DBJ_ROI_MAX_ZONES   8       /* 채널당 침대 ROI 상한 */
/* roi_id 자리에 들어가는 특수값.
 *  - ROI_CLEAR   : 그 채널의 ROI를 전부 삭제
 *  - RISK_UPDATE : 그 채널의 모든 침대에 같은 위험도 일괄 적용
 *  - 이벤트 헤더 : 발생 침대를 특정하지 못함(미상) */
#define DBJ_ROI_ID_ALL      0xFF
#define DBJ_ROI_ID_NONE     0xFF
/* reserved ↔ roi_id 변환 (ROI_SET / ROI_CLEAR / RISK_UPDATE 전용) */
#define DBJ_CTRL_ROI_ID(reserved_)  ((uint8_t)((reserved_) & 0xFF))

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
    uint16_t reserved;     /* 기본 0. CAMERA_SET 시엔 이어지는 URL 문자열 바이트 길이.
                            * ROI_SET/ROI_CLEAR/RISK_UPDATE 시엔 하위 8비트가 roi_id */
} dbj_ctrl_header_t;       /* 8바이트, 이어서 point_count개의 dbj_roi_point_t 또는 reserved 바이트의 URL */

typedef struct {
    uint16_t x;            /* 정규화 x × DBJ_ROI_COORD_SCALE (0~10000) */
    uint16_t y;            /* 정규화 y × DBJ_ROI_COORD_SCALE (0~10000) */
} dbj_roi_point_t;         /* 4바이트 */

typedef struct {
    uint8_t  roi_id;       /* 이 채널 안의 침대 번호 0~7 */
    uint8_t  reserved;     /* 0 */
    uint32_t resident_id;  /* residents.resident_id (0 = 매핑 해제) */
} dbj_roi_bind_t;          /* 6바이트, ROI_BIND 시 헤더 뒤에 1개 */

typedef struct {
    uint8_t brightness;    /* 0~100 (서버가 카메라 실제 범위로 매핑) */
    uint8_t contrast;      /* 0~100 */
    uint8_t saturation;    /* 0~100 */
} dbj_image_params_t;      /* 3바이트, IMAGE_SET 시 헤더 뒤에 1개 */

typedef struct {
    uint8_t  mode;         /* DBJ_FOCUS_WHOLE | DBJ_FOCUS_AREA */
    uint8_t  reserved;     /* 0 */
    uint16_t x;            /* AREA 초점 시 클릭 중심 정규화 x × DBJ_ROI_COORD_SCALE */
    uint16_t y;            /* 정규화 y × DBJ_ROI_COORD_SCALE */
} dbj_focus_t;             /* 6바이트, FOCUS_SET 시 헤더 뒤에 1개 */

typedef struct {
    uint16_t magic;        /* DBJ_EVT_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  type;         /* DBJ_EVT_* */
    uint8_t  channel;      /* 발생 채널 0~3 */
    uint8_t  roi_id;       /* 발생 침대 0~7. 어느 침대인지 못 밝히면 DBJ_ROI_ID_NONE */
    uint16_t x;            /* 발생 위치 정규화 x × DBJ_ROI_COORD_SCALE (없으면 0) */
    uint16_t y;            /* 발생 위치 정규화 y × DBJ_ROI_COORD_SCALE (없으면 0) */
    uint64_t timestamp_ms; /* 서버 Unix time (밀리초) */
} dbj_evt_header_t;        /* 18바이트, 페이로드 없음 */

typedef struct {
    uint16_t magic;        /* DBJ_SEARCH_MAGIC */
    uint8_t  version;      /* DBJ_VS_VERSION */
    uint8_t  channel;      /* 요청한 검색 대상 채널 0~3 */
    uint32_t text_len;     /* 이어지는 UTF-8 답변 텍스트 바이트 수 */
} dbj_search_result_header_t;  /* 8바이트, 이어서 text_len 바이트 UTF-8 텍스트 */

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* DBJ_VIDEO_STREAM_H */
