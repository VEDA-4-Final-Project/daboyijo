#ifndef STREAMPROTOCOL_H
#define STREAMPROTOCOL_H

// 영상 서버(Pi) ↔ Qt 관제 클라이언트 사이의 TCP 바이너리 프로토콜.
// 서버 protocol/video_stream.h와 반드시 동일하게 유지할 것.
// (예전엔 mainwindow.h 안에 있었다 — 통신을 StreamClient로 분리하면서 UI와 무관한
//  규격만 이 파일로 뺐다. UI가 QML로 바뀌어도 이 파일은 그대로 쓴다.)

#include <cstdint>

// 🌟 명세서 3번에 정의된 16바이트 리틀엔디언 구조체 그대로 구현
// (서버 protocol/video_stream.h와 반드시 동일하게 유지할 것)
#pragma pack(push, 1)
struct dbj_vs_header_t {
    uint16_t magic;         // 2B (0xDB4B)
    uint8_t version;        // 1B (0x01)
    uint8_t channel;        // 1B (0~3)
    uint64_t timestamp_ms;  // 8B (Unix epoch 밀리초)
    uint32_t payload_len;   // 4B (JPEG 이미지 크기)
};

// 역방향(클라→서버) 제어 메시지 — 침대 ROI / 낙상 확인 전송용. magic=0xDB4C.
struct dbj_ctrl_header_t {
    uint16_t magic;         // 2B (0xDB4C)
    uint8_t version;        // 1B (0x01)
    uint8_t type;           // 1B (아래 kCtrl* 상수 참조)
    uint8_t channel;        // 1B (0~3)
    uint8_t point_count;    // 1B (이어지는 점 개수 / 위험도 값)
    uint16_t reserved;      // 2B (0, CAMERA_SET에선 URL 바이트 길이)
};
struct dbj_roi_point_t {
    uint16_t x;             // 정규화 x × 10000 (0~10000)
    uint16_t y;             // 정규화 y × 10000 (0~10000)
};
// 침대 ↔ 입소자 매핑 — ROI_BIND 헤더 뒤 1개.
struct dbj_roi_bind_t {
    uint8_t  roi_id;        // 채널 안 침대 번호 0~7
    uint8_t  reserved;      // 0
    uint32_t resident_id;   // residents.resident_id (0 = 매핑 해제)
};                          // 6B
// 카메라 이미지 파라미터 (밝기/대비/채도) — IMAGE_SET 헤더 뒤 1개. 0~100.
struct dbj_image_params_t {
    uint8_t brightness;     // 0~100 (서버가 카메라 실제 범위로 매핑)
    uint8_t contrast;       // 0~100
    uint8_t saturation;     // 0~100
};                          // 3B
// 카메라 포커스 — FOCUS_SET 헤더 뒤 1개.
struct dbj_focus_t {
    uint8_t  mode;          // 0=전체 자동초점, 1=클릭 영역 초점
    uint8_t  reserved;      // 0
    uint16_t x;             // 클릭 중심 정규화 x × 10000
    uint16_t y;             // 정규화 y × 10000
};                          // 6B

// 역방향(서버→클라) 이벤트 알림 — 낙상. magic=0xDB4D. 페이로드 없이 헤더(18B)만.
// 영상 프레임(0xDB4B)과 같은 소켓(5500)으로 섞여 들어오며, magic으로 구분한다.
struct dbj_evt_header_t {
    uint16_t magic;         // 2B (0xDB4D)
    uint8_t  version;       // 1B (0x01)
    uint8_t  type;          // 1B (0x01 = 낙상 확정)
    uint8_t  channel;       // 1B (0~3, 발생 채널)
    uint8_t  roi_id;        // 1B (발생 침대 0~7, 특정 못 하면 kRoiIdNone)
    uint16_t x;             // 2B (발생 위치 정규화 x ×10000)
    uint16_t y;             // 2B (발생 위치 정규화 y ×10000)
    uint64_t timestamp_ms;  // 8B (서버 Unix time ms)
};                          // 18B

// 역방향(서버→클라) 영상검색 결과 — magic=0xDB4E. 헤더 뒤 UTF-8 텍스트(text_len 바이트).
struct dbj_search_result_header_t {
    uint16_t magic;         // 2B (0xDB4E)
    uint8_t  version;       // 1B (0x01)
    uint8_t  channel;       // 1B (0~3, 요청한 채널)
    uint32_t text_len;      // 4B (이어지는 UTF-8 답변 텍스트 바이트 수)
};                          // 8B
#pragma pack(pop)

// 제어 메시지 상수 (서버와 합의된 값 — protocol/video_stream.h와 동일하게 유지)
static constexpr uint16_t kCtrlMagic = 0xDB4C;
static constexpr uint8_t kCtrlRoiSet = 0x01;
static constexpr uint8_t kCtrlRoiClear = 0x02;
static constexpr uint8_t kCtrlCameraSet = 0x05;    // 채널 카메라 연결 (헤더 뒤 RTSP URL)
static constexpr uint8_t kCtrlCameraClear = 0x06;  // 채널 카메라 해제
static constexpr uint8_t kCtrlImageSet = 0x07;     // 카메라 이미지 파라미터 (헤더 뒤 dbj_image_params_t)
static constexpr uint8_t kCtrlFocusSet = 0x08;     // 카메라 포커스 (헤더 뒤 dbj_focus_t)
static constexpr uint8_t kCtrlRoiBind = 0x09;      // 침대 ↔ 입소자 매핑 (헤더 뒤 dbj_roi_bind_t)
static constexpr uint8_t kCtrlSearchQuery = 0x0A;  // 영상검색 질의 (헤더 뒤 UTF-8 질의 문자열)
static constexpr int kSearchQueryMax = 300;        // DBJ_SEARCH_QUERY_MAX
static constexpr uint8_t kChannelAll = 0xFF;       // SEARCH_QUERY channel 자리의 "전체 채널" (DBJ_CHANNEL_ALL)
static constexpr uint8_t kFocusWhole = 0;          // 전체 자동초점
static constexpr uint8_t kFocusArea = 1;           // 클릭 영역 초점
static constexpr int kRoiCoordScale = 10000;
// roi_id 자리의 특수값 — 삭제 시 "그 채널 전부", 이벤트에선 "침대 미상".
// (서버 DBJ_ROI_ID_ALL / DBJ_ROI_ID_NONE 과 같은 값으로 유지할 것)
static constexpr int kRoiIdAll = 0xFF;
static constexpr int kRoiIdNone = 0xFF;
static constexpr int kCameraUrlMax = 512;          // DBJ_CAMERA_URL_MAX

// 이벤트 메시지 상수 (서버 스펙)
static constexpr uint16_t kEvtMagic = 0xDB4D;
static constexpr uint8_t kEvtFall = 0x01;       // 낙상 확정
static constexpr uint8_t kEvtBedEgress = 0x02;  // 침대 이탈
static constexpr uint8_t kEvtVitalAbnormal = 0x03;  // 웨어러블 생체데이터 이상 (x,y 미사용)

// 영상검색 결과 메시지 상수 (서버 스펙)
static constexpr uint16_t kSearchMagic = 0xDB4E;

// 통합 경보 해제(모자이크 복구) / 환자 위험도 — 예전엔 mainwindow.cpp 안에 흩어져 있던 값.
static constexpr uint8_t kCtrlAlarmConfirm = 0x03;
static constexpr uint8_t kCtrlRiskLevel = 0x04;    // point_count=위험도(1하·2중·3상), reserved 하위 8비트=침대

#endif // STREAMPROTOCOL_H
