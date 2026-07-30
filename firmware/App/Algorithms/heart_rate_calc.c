#include "heart_rate_calc.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

/* -----------------------------------------------------------------
 * [시스템 상수 및 알고리즘 고도화 튜닝 파라미터]
 * ----------------------------------------------------------------- */
#define SAMPLING_FREQ              50       // 시스템 샘플링 주파수 (50Hz = 20ms)
#define PRINT_INTERVAL_SAMPLES     50       // 디버그 로그 출력 주기 (50샘플 = 1초)

#define PPG_AIR_THRESHOLD          25000UL  // 허공 감지 기준선 (이 값 미만은 착용 해제)
#define WEAR_DEBOUNCE_SAMPLES      50       // 탈착 확정 가드 타임 (1초)
#define DESK_TIMEOUT_SAMPLES       500      // 바닥 방치(무기물) 록아웃 타임 (10초)

#define MOTION_BLANKING_THRESHOLD  130.0f   // 모션 블랭킹 임계값 (자이로 절대합)
#define BLANKING_WINDOW_SAMPLES    50       // 모션 감지 후 동결 시간 (1초)

#define SAMPLE_BUFFER_SIZE         100      // 파형 분석용 로컬 버퍼 크기
#define MIN_PEAK_DISTANCE          22       // 💡 [반사파 차단] 불응기 22샘플 확장 (440ms 동결로 더블 피크 완전 저지)
#define AC_PEAK_MIN_THRESHOLD      20.0f    // LPF 감쇠를 반영한 피크 인식 최소 진폭
#define STABLE_PEAK_REQUIRED       6        // 최종 생체 록(Lock) 진입에 필요한 연속 검증 스텝

/* -----------------------------------------------------------------
 * [모듈 내부 정적(static) 컨텍스트 제어 소자]
 * ----------------------------------------------------------------- */
static HRState_t s_hr_state = HR_STAT_NONE;
static uint8_t  s_is_worn = 0;
static uint8_t  s_prev_worn = 0;

static uint32_t s_current_bpm = 0;
static uint32_t s_current_spo2 = 0;
static uint32_t s_blanking_counter = 0;
static uint16_t s_print_counter = 0;

/* 착용 상태 머신 제어 카운터 */
static uint16_t s_air_counter = 0;
static uint16_t s_desk_counter = 0;
static uint8_t  s_is_desk_locked = 0;
static uint8_t  s_stable_peak_counter = 0;

/* 시계열 피크 분석 엔진 소자 */
static uint32_t s_last_peak_time = 0;
static uint32_t s_sample_tick = 0;
static uint32_t s_prev_interval = 0;

/* 디지털 필터 프레임워크 (💡 6차 LPF 버퍼 및 제어 소자 일치화 완료) */
static float s_dc_est_ir = 0.0f;
static float s_dc_est_red = 0.0f;
static float s_lpf_buffer_ir[6] = {0.0f};
static float s_lpf_buffer_red[6] = {0.0f};
static uint8_t s_lpf_idx_ir = 0;
static uint8_t s_lpf_idx_red = 0;

/* 실시간 파형 저장용 링 버퍼 */
static float s_ppg_buffer_ir[SAMPLE_BUFFER_SIZE] = {0.0f};
static uint16_t s_buf_idx = 0;

/* SpO2 정밀 연산용 사이클 내부 파형 트래커 */
static float s_ir_ac_max = -999999.0f;
static float s_ir_ac_min = 999999.0f;
static float s_red_ac_max = -999999.0f;
static float s_red_ac_min = 999999.0f;

/**
  * @brief 심박 및 산소포화도 연산 모듈 초기화
  */
void HeartRateCalc_Init(void)
{
    HeartRateCalc_Reset();
    s_is_desk_locked = 0;
    printf("[ HEALTH ] Advanced Pulse & SpO2 Integration Engine Initialized.\r\n");
}

/**
  * @brief 코어 연산 파이프라인 (인터럽트 외부 백그라운드 루프 호출 권장)
  */
void HeartRateCalc_Process_DMA(MAX30102_Data_t *sensor_data, float gyro_x, float gyro_y, float gyro_z)
{
    if (sensor_data == NULL) return;

    /* -----------------------------------------------------------------
     * [1단계] 2중 필터 기반 고정밀 착용 상태 머신 (Air vs. Desk 감지)
     * ----------------------------------------------------------------- */
    if (sensor_data->ir < PPG_AIR_THRESHOLD)
    {
        s_desk_counter = 0;
        s_stable_peak_counter = 0;

        if (s_is_worn == 1 || s_is_desk_locked == 1)
        {
            s_air_counter++;
            if (s_air_counter >= WEAR_DEBOUNCE_SAMPLES)
            {
                s_is_worn = 0;
                s_hr_state = HR_STAT_NONE;
                s_air_counter = 0;

                if (s_is_desk_locked) {
                    s_is_desk_locked = 0;
                    printf("[ WEAR ALERT ] 🛑 무기물/바닥 방치 해제 (기기 움직임 포착)\r\n");
                } else {
                    printf("[ WEAR ALERT ] 🛑 탈착 상태 진입 (허공)\r\n");
                }
            }
        }
    }
    else
    {
        s_air_counter = 0;
        if (s_is_desk_locked) goto SYSTEM_PRINT_LOOP;

        if (s_is_worn == 0)
        {
            s_is_worn = 1;
            s_hr_state = HR_STAT_ACQUIRING;
            s_desk_counter = 0;
            s_stable_peak_counter = 0;
            printf("[ WEAR ALERT ] 🔔 착용 감지 (생체 신호 탐색 시작)\r\n");
        }
    }

    /* 하강 에지 발생 시 내부 메모리 즉각 리셋 및 잡음 오염 방지 */
    if (s_prev_worn == 1 && s_is_worn == 0) HeartRateCalc_Reset();
    s_prev_worn = s_is_worn;

    if (!s_is_worn) goto SYSTEM_PRINT_LOOP;

    /* -----------------------------------------------------------------
     * [2단계] IMU 연동 동적 잡음 억제 (Motion Blanking Engine)
     * ----------------------------------------------------------------- */
    float motion_intensity = fabsf(gyro_x) + fabsf(gyro_y) + fabsf(gyro_z);
    if (motion_intensity > MOTION_BLANKING_THRESHOLD)
    {
        s_blanking_counter = BLANKING_WINDOW_SAMPLES;
    }

    if (s_blanking_counter > 0)
    {
        s_blanking_counter--;
        // 모션 노이즈 발생 시 신호 왜곡을 방지하기 위해 버퍼 누적 및 트래킹만 초기화하고 연산 점프
        s_ir_ac_max = -999999.0f; s_ir_ac_min = 999999.0f;
        s_red_ac_max = -999999.0f; s_red_ac_min = 999999.0f;
        goto SYSTEM_PRINT_LOOP;
    }

    /* -----------------------------------------------------------------
     * [3단계] 고성능 디지털 필터 파이프라인 (💡 6차 LPF 완전 교정 완료)
     * ----------------------------------------------------------------- */
    // --- (1) IR 채널 신호 정제 ---
    if (s_dc_est_ir == 0.0f) s_dc_est_ir = (float)sensor_data->ir;
    s_dc_est_ir = (s_dc_est_ir * 0.99f) + ((float)sensor_data->ir * 0.01f); // 초고정밀 DC 추출 필터
    float ac_raw_ir = (float)sensor_data->ir - s_dc_est_ir;

    s_lpf_buffer_ir[s_lpf_idx_ir] = ac_raw_ir;
    s_lpf_idx_ir = (s_lpf_idx_ir + 1) % 6; // 💡 4 -> 6 교정

    float filtered_ppg_ir = 0.0f;
    for (int i = 0; i < 6; i++) {
        filtered_ppg_ir += s_lpf_buffer_ir[i];
    }
    filtered_ppg_ir /= 6.0f; // 💡 6차 이동평균 연산 일치화

    s_ppg_buffer_ir[s_buf_idx] = filtered_ppg_ir;
    s_buf_idx = (s_buf_idx + 1) % SAMPLE_BUFFER_SIZE;

    // --- (2) RED 채널 신호 정제 ---
    if (s_dc_est_red == 0.0f) s_dc_est_red = (float)sensor_data->red;
    s_dc_est_red = (s_dc_est_red * 0.99f) + ((float)sensor_data->red * 0.01f);
    float ac_raw_red = (float)sensor_data->red - s_dc_est_red;

    s_lpf_buffer_red[s_lpf_idx_red] = ac_raw_red;
    s_lpf_idx_red = (s_lpf_idx_red + 1) % 6; // 💡 4 -> 6 교정

    float filtered_ppg_red = 0.0f;
    for (int i = 0; i < 6; i++) {
        filtered_ppg_red += s_lpf_buffer_red[i];
    }
    filtered_ppg_red /= 6.0f; // 💡 6차 이동평균 연산 일치화

    // --- (3) 실시간 한 주기 내 극대/극소 진폭 트래킹 ---
    if (filtered_ppg_ir > s_ir_ac_max)   s_ir_ac_max = filtered_ppg_ir;
    if (filtered_ppg_ir < s_ir_ac_min)   s_ir_ac_min = filtered_ppg_ir;
    if (filtered_ppg_red > s_red_ac_max) s_red_ac_max = filtered_ppg_red;
    if (filtered_ppg_red < s_red_ac_min) s_red_ac_min = filtered_ppg_red;

    /* 바닥 무기물 방치 감시 카운터 기본 누적 */
    if (s_hr_state == HR_STAT_ACQUIRING)
    {
        s_desk_counter++;
        if (s_desk_counter >= DESK_TIMEOUT_SAMPLES)
        {
            s_is_worn = 0;
            s_hr_state = HR_STAT_NONE;
            s_is_desk_locked = 1;
            printf("[ WEAR ALERT ] 🛑 탈착 판정 (바닥 방치 오작동 방지 락 해제)\r\n");
            goto SYSTEM_PRINT_LOOP;
        }
    }

    /* -----------------------------------------------------------------
     * [4단계] 시계열 로컬 맥박 피크 디텍터 및 노이즈 차단 엔진
     * ----------------------------------------------------------------- */
    s_sample_tick++;

    uint16_t prev_idx  = (s_buf_idx + SAMPLE_BUFFER_SIZE - 2) % SAMPLE_BUFFER_SIZE;
    uint16_t curr_idx  = (s_buf_idx + SAMPLE_BUFFER_SIZE - 1) % SAMPLE_BUFFER_SIZE;
    uint16_t prev2_idx = (s_buf_idx + SAMPLE_BUFFER_SIZE - 3) % SAMPLE_BUFFER_SIZE;

    // 3점 비교법 기반 극대점(Peak) 검출 수행
    if ((s_sample_tick - s_last_peak_time >= MIN_PEAK_DISTANCE) &&
        s_ppg_buffer_ir[prev_idx] > AC_PEAK_MIN_THRESHOLD &&
        s_ppg_buffer_ir[prev_idx] > s_ppg_buffer_ir[curr_idx] &&
        s_ppg_buffer_ir[prev_idx] > s_ppg_buffer_ir[prev2_idx])
    {
        uint32_t interval = s_sample_tick - s_last_peak_time;

        // 피크성 신호 감지 시 바닥 방치 타이머 즉시 완화 리셋
        s_desk_counter = 0;

        if (s_last_peak_time == 0)
        {
            s_last_peak_time = s_sample_tick;
            s_stable_peak_counter = 0;
            s_prev_interval = 0;
        }
        else
        {
            uint32_t inst_bpm = (60 * SAMPLING_FREQ) / interval;

            if (inst_bpm >= 40 && inst_bpm <= 180)
            {
                int32_t interval_diff = (int32_t)interval - (int32_t)s_prev_interval;

                // [오류 원천 봉쇄] 탐색 단계(ACQUIRING)에서는 마진을 넓게(±12샘플), 완료 단계(VALID)에서는 깐깐하게(±6샘플) 가변 제어
                uint32_t allowed_margin = (s_hr_state == HR_STAT_VALID) ? 6 : 12;

                if (s_prev_interval == 0 || labs(interval_diff) <= (long)allowed_margin)
                {
                    s_last_peak_time = s_sample_tick;
                    s_prev_interval = interval;

                    /* -------------------------------------------------
                     * 💡 [핵심 구현] 실제 데이터 기반 Ratio-of-Ratios SpO2 연산
                     * ------------------------------------------------- */
                    float ac_ir = s_ir_ac_max - s_ir_ac_min;
                    float ac_red = s_red_ac_max - s_red_ac_min;
                    float dc_ir = s_dc_est_ir;
                    float dc_red = s_dc_est_red;

                    if (ac_ir > 1.0f && ac_red > 1.0f && dc_ir > 1000.0f && dc_red > 1000.0f)
                    {
                        float r = (ac_red / dc_red) / (ac_ir / dc_ir);
                        float inst_spo2 = 104.0f - 17.0f * r;

                        // 의학적 한계선 클램핑 가드 가동
                        if (inst_spo2 > 100.0f) inst_spo2 = 100.0f;
                        if (inst_spo2 < 70.0f)  inst_spo2 = 70.0f;

                        // 생리학적 특성을 고려한 95:5 비율 초정밀 무거운 EMA 스무딩 필터 적용
                        if (s_current_spo2 == 0 || s_hr_state != HR_STAT_VALID) {
                            s_current_spo2 = (uint32_t)inst_spo2;
                        } else {
                            s_current_spo2 = (s_current_spo2 * 95 + (uint32_t)inst_spo2 * 5) / 100;
                        }
                    }

                    // 생체 검증 스텝 승격 연산
                    if (s_hr_state == HR_STAT_ACQUIRING)
                    {
                        s_stable_peak_counter++;
                        if (s_stable_peak_counter >= STABLE_PEAK_REQUIRED)
                        {
                            s_hr_state = HR_STAT_VALID;
                        }
                    }

                    // 신속한 반응성을 위한 7:3 구조의 BPM 지수 필터링
                    if (s_current_bpm == 0 || s_hr_state != HR_STAT_VALID) {
                        s_current_bpm = inst_bpm;
                    } else {
                        s_current_bpm = (s_current_bpm * 7 + inst_bpm * 3) / 10;
                    }
                }
                else
                {
                    /*
                     * 💡 [버그 원천 차단] 노이즈 전염(Cascading Failure) 완전 해결책
                     * 신호 획득 중 간격이 뒤틀리면, 오염된 가짜 간격을 저장하지 않고 기준점을 0으로 완전 초기화(Flush)하여
                     * 바로 다음 진입할 진짜 피크 간격 연산에 전혀 해를 끼치지 못하도록 완벽하게 방어 차단합니다.
                     */
                    s_last_peak_time = s_sample_tick;

                    if (s_hr_state == HR_STAT_VALID) {
                        s_prev_interval = interval;
                        if (s_stable_peak_counter > 0) s_stable_peak_counter--;
                        s_hr_state = HR_STAT_ACQUIRING;
                    } else {
                        s_prev_interval = 0;
                        s_stable_peak_counter = 0;
                    }
                }

                // 다음 맥동 사이클의 정밀 진폭 분석을 위한 트래커 리셋
                s_ir_ac_max = -999999.0f; s_ir_ac_min = 999999.0f;
                s_red_ac_max = -999999.0f; s_red_ac_min = 999999.0f;
            }
            else
            {
                // 생체 신호 외 범위 아웃 시 완전 리프레시 수행
                s_last_peak_time = s_sample_tick;
                s_stable_peak_counter = 0;
                s_prev_interval = 0;
                if (s_hr_state == HR_STAT_VALID) s_hr_state = HR_STAT_ACQUIRING;

                s_ir_ac_max = -999999.0f; s_ir_ac_min = 999999.0f;
                s_red_ac_max = -999999.0f; s_red_ac_min = 999999.0f;
            }
        }
    }

SYSTEM_PRINT_LOOP:

    /* -----------------------------------------------------------------
     * [5단계] 1초 주기 시스템 정기 로깅 UI 엔진
     * ----------------------------------------------------------------- */
    s_print_counter++;
    if (s_print_counter >= PRINT_INTERVAL_SAMPLES)
    {
        s_print_counter = 0;

        if (s_is_worn && s_hr_state == HR_STAT_VALID)
        {
            printf("[DISPLAY] ❤️ 심박수: %lu BPM | 🩺 산소포화도: %lu%%\r\n",
                   (unsigned long)s_current_bpm, (unsigned long)s_current_spo2);
        }
        else if (s_is_worn && s_hr_state == HR_STAT_ACQUIRING)
        {
            printf("[DISPLAY] ⏳ 생체 신호 동기화 중... (검증 단계: %d/%d)\r\n",
                   s_stable_peak_counter, STABLE_PEAK_REQUIRED);
        }
        else if (!s_is_desk_locked)
        {
            printf("[DISPLAY] ⚠️ 기기 미착용 (신호 대기 모드)\r\n");
        }
    }
}

/**
  * @brief 내부 연산 파라미터 및 컨텍스트 클리어
  */
void HeartRateCalc_Reset(void)
{
    s_hr_state = HR_STAT_NONE;
    s_is_worn = 0;
    s_current_bpm = 0;
    s_current_spo2 = 0;
    s_blanking_counter = 0;
    s_print_counter = 0;
    s_stable_peak_counter = 0;
    s_desk_counter = 0;

    s_last_peak_time = 0;
    s_sample_tick = 0;
    s_prev_interval = 0;

    s_dc_est_ir = 0.0f;
    s_dc_est_red = 0.0f;
    s_lpf_idx_ir = 0;
    s_lpf_idx_red = 0;
    s_buf_idx = 0;

    s_ir_ac_max = -999999.0f; s_ir_ac_min = 999999.0f;
    s_red_ac_max = -999999.0f; s_red_ac_min = 999999.0f;

    // 💡 6차 필터 버퍼 초기화 일치화 완료 (4 -> 6)
    for (int i = 0; i < 6; i++) {
        s_lpf_buffer_ir[i] = 0.0f;
        s_lpf_buffer_red[i] = 0.0f;
    }
    for (int i = 0; i < SAMPLE_BUFFER_SIZE; i++) s_ppg_buffer_ir[i] = 0.0f;
}

uint8_t  HeartRateCalc_IsWorn(void)  { return s_is_worn; }
uint32_t HeartRateCalc_GetBPM(void)   { return (s_hr_state == HR_STAT_VALID) ? s_current_bpm : 0; }
uint32_t HeartRateCalc_GetSpO2(void)  { return (s_hr_state == HR_STAT_VALID) ? s_current_spo2 : 0; }
