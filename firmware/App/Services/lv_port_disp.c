#include "lv_port_disp.h"
#include "lvgl.h"
#include "st7789.h" // ST7789 드라이버 헤더

#define MY_DISP_HOR_RES 240
#define MY_DISP_VER_RES 280

// 버퍼 크기 설정 (RGB565 기준: 240px * 20줄 * 2Bytes)
#define BUF_SIZE_IN_BYTES (MY_DISP_HOR_RES * 20 * sizeof(lv_color_t))
static uint8_t buf_1[BUF_SIZE_IN_BYTES];

// LVGL v9.1 flush 콜백 함수
static void disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    uint16_t x1 = area->x1;
    uint16_t y1 = area->y1;
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;
    uint32_t len = (uint32_t)w * h;

    // 💡 16비트 바이트 스왑 (상위/하위 바이트 교체)
    // SPI 전송 전 픽셀 데이터의 바이트 순서를 뒤집어 폰트 잔영/노이즈를 제거합니다.
    uint16_t * buf16 = (uint16_t *)px_map;
    for (uint32_t i = 0; i < len; i++) {
        buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
    }

    // ST7789 화면 전송 함수 호출
    ST7789_DrawImage(x1, y1, w, h, buf16);

    // 전송 완료 알림
    lv_display_flush_ready(disp);
}

void lv_port_disp_init(void)
{
    // 1. LVGL v9.1 디스플레이 객체 생성
    lv_display_t * disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);

    // 2. 표준 16비트 RGB565 컬러 포맷 지정
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    // 3. 드로잉 버퍼 설정
    lv_display_set_buffers(disp, buf_1, NULL, sizeof(buf_1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 4. flush 콜백 함수 등록
    lv_display_set_flush_cb(disp, disp_flush);
}
