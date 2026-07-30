#include "heart_rate_calc.h"
#include <math.h>
#include <stdio.h>
#include "stm32f4xx_hal.h"

#define MOTION_THRESHOLD     7.0f
#define HOLD_DELAY_SAMPLES   10

// [심박수 계산용 디파인]
#define PEAK_THRESHOLD       30.0f   // 0을 기준으로 출렁이는 순수 맥파용 문턱값
#define MIN_RR_INTERVAL      350     // 350ms (최대 약 170 BPM 제한)
#define MAX_RR_INTERVAL      1500    // 1500ms (최소 40 BPM 제한)
#define STABILIZE_SAMPLES    75      // 손가락 접촉 후 필터 안정화까지 걸리는 샘플 수 (1.5초)

static float sg_dc_red = 0.0f;
static float sg_lpf_red = 0.0f;
static uint32_t s_last_print_time = 0;
static uint32_t s_motion_hold_counter = 0;
static uint8_t s_was_frozen = 0;

// [안정화 카운터 및 심박 변수]
static uint32_t s_stable_counter = 0;
static uint32_t s_current_bpm = 0;
static uint32_t s_last_peak_time = 0;
static float s_prev_lpf_red = 0.0f;
static float s_prev_prev_lpf_red = 0.0f;

// 이벤트 상태 추적용 플래그
static uint8_t s_finger_attached = 0;       // 1: 손가락 접촉 중, 0: 떨어짐
static uint8_t s_stabilization_logged = 0;  // 초기 필터 안정화 완료 로그 플래그
static uint8_t s_bpm_output_logged = 0;     // 첫 심박수 확정 로그 플래그

void HeartRateCalc_Init(void) {
    HeartRateCalc_Reset();
}

void HeartRateCalc_Reset(void) {
    sg_dc_red = 0.0f;
    sg_lpf_red = 0.0f;
    s_motion_hold_counter = 0;
    s_was_frozen = 0;
    s_stable_counter = 0;   // 안정화 카운터 리셋

    s_current_bpm = 0;
    s_last_peak_time = 0;
    s_prev_lpf_red = 0.0f;
    s_prev_prev_lpf_red = 0.0f;

    s_last_print_time = HAL_GetTick();

    // 👉 리셋 시 상태 로그 플래그도 초기화 (s_finger_attached는 Update 함수에서 제어)
    s_stabilization_logged = 0;
    s_bpm_output_logged = 0;
}

uint32_t HeartRateCalc_GetBPM(void) {
    return s_current_bpm;
}

uint32_t HeartRateCalc_GetSpO2(void) {
    return 0;
}

HRState_t HeartRateCalc_Update(MAX30102_Data_t *sensor_data, float gyro_x, float gyro_y, float gyro_z) {
    if (sensor_data == NULL) return HR_STAT_NONE;

    float raw_red = (float)sensor_data->red;
    uint32_t current_time = HAL_GetTick();

    // 손가락 탈착 감지 (30000 미만이면 즉시 초기화)
    if (raw_red < 30000.0f) {
        // 👉 [로그 4] 손가락 떼짐 감지 (기존에 붙어있던 상태였을 때만 1번 출력)
        if (s_finger_attached) {
            printf("\r\n[ EVENT ] === 손가락 탈착 감지: 측정 종료 ===\r\n\r\n");
            s_finger_attached = 0;
        }

        HeartRateCalc_Reset();

        if (current_time - s_last_print_time >= 5000) {
            s_last_print_time = current_time;
            printf("0,0.0,0,0.0\r\n");
        }
        return HR_STAT_NONE;
    }

    // 👉 [로그 1] 손가락 처음 댐 감지
    if (!s_finger_attached) {
        printf("\r\n[ EVENT ] === 손가락 접촉 감지: 측정 시작 (필터 안정화 중...) ===\r\n");
        s_finger_attached = 1;
    }

    // 모션 강도 계산
    float motion_intensity = fabs(gyro_x) + fabs(gyro_y) + fabs(gyro_z);

    if (motion_intensity > MOTION_THRESHOLD) {
        s_motion_hold_counter = HOLD_DELAY_SAMPLES;
    } else if (s_motion_hold_counter > 0) {
        s_motion_hold_counter--;
    }

    // 움직임 제어 및 필터 적용
    if (s_motion_hold_counter > 0) {
        s_was_frozen = 1;
    } else {
        // 1. 최초 진입 시 하드웨어 첫 값으로 무조건 동기화
        if (sg_dc_red < 1000.0f) {
            sg_dc_red = raw_red;
            sg_lpf_red = 0.0f;
            s_prev_lpf_red = 0.0f;
            s_prev_prev_lpf_red = 0.0f;
            s_stable_counter = 0;
            s_was_frozen = 0;
        }

        // 2. 타임아웃 기반 Fast-Lock 및 일반 모드 전환
        if (s_was_frozen || s_stable_counter < STABILIZE_SAMPLES) {
            sg_dc_red = (0.85f * sg_dc_red) + (0.15f * raw_red);
            if (!s_was_frozen) {
                s_stable_counter++;

                // 👉 [로그 2] 초기 1.5초(75샘플) 하드웨어 필터 안정화 완료 시점 포착
                if (s_stable_counter >= STABILIZE_SAMPLES && !s_stabilization_logged) {
                    printf("[ EVENT ] === 필터 초기 안정화 완료: 신호 수집 및 심박원 추적 시작 ===\r\n");
                    s_stabilization_logged = 1;
                }
            }
            s_was_frozen = 0;
        } else {
            sg_dc_red = (0.97f * sg_dc_red) + (0.03f * raw_red);
        }

        // 순수 AC 맥파 신호 추출
        float ac_red = raw_red - sg_dc_red;

        // 고주파 노이즈 제거 LPF
        sg_lpf_red = (0.75f * sg_lpf_red) + (0.25f * ac_red);

        // -----------------------------------------------------------------
        // [기울기 기반 피크 검출 로직]
        // -----------------------------------------------------------------
        if (s_prev_lpf_red > s_prev_prev_lpf_red && s_prev_lpf_red > sg_lpf_red) {
            if (s_stable_counter >= STABILIZE_SAMPLES) {
                if (s_prev_lpf_red > PEAK_THRESHOLD && s_prev_lpf_red < 1500.0f) {
                    uint32_t rr_interval = current_time - s_last_peak_time;

                    if (rr_interval >= MIN_RR_INTERVAL && rr_interval <= MAX_RR_INTERVAL) {
                        uint32_t calculated_bpm = 60000 / rr_interval;

                        if (s_current_bpm == 0) {
                            s_current_bpm = calculated_bpm;
                        } else {
                            s_current_bpm = (s_current_bpm * 3 + calculated_bpm) / 4;
                        }

                        // 👉 [로그 3] 첫 유효 피크 수집 완료 및 유효 심박수(BPM) 최초 출력 시점
                        if (!s_bpm_output_logged) {
                            printf("[ EVENT ] === 유효 맥박 검출 성공: 본격적인 심박수 데이터 계산 시작 ===\r\n");
                            s_bpm_output_logged = 1;
                        }
                    }
                    s_last_peak_time = current_time;
                }
            }
        }

        s_prev_prev_lpf_red = s_prev_lpf_red;
        s_prev_lpf_red = sg_lpf_red;
    }

    // 5초 주기 출력
    if (current_time - s_last_print_time >= 5000) {
        s_last_print_time = current_time;

        printf("%lu,%.1f,%lu,%.1f\r\n",
               (uint32_t)raw_red,
               sg_lpf_red,
               s_current_bpm,
               motion_intensity);
    }

    return HR_STAT_VALID;
}
