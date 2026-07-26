#include "bio_manager.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define WINDOW_SIZE        200  // 20ms * 200 = 4000ms (4초)
#define TICK_INTERVAL_4S   200  // 20ms 스케줄러 기준 4초를 세기 위한 틱 (20ms * 200 = 4000ms)

extern MAX30102_t hmax;

// 모듈 내부 정적 변수 관리
static uint32_t g_red_buffer[WINDOW_SIZE];
static uint32_t g_ir_buffer[WINDOW_SIZE];
static uint16_t g_filled_samples = 0;
static uint16_t g_tick_counter = 0;

// 내부 연산용 정적 헬퍼 함수 선언
static void Calculate_Bio_Metrics(float *out_hr, float *out_spo2);

void BioManager_Init(void)
{
    g_filled_samples = 0;
    g_tick_counter = 0;

    // 버퍼 초기화
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        g_red_buffer[i] = 0;
        g_ir_buffer[i] = 0;
    }

    printf("[ BIO ] Manager Initialized.\r\n");
}

void BioManager_Update(void)
{
    MAX30102_Data_t raw_data;

    // 1. FIFO에 밀려있는 모든 데이터를 수거하여 슬라이딩 윈도우에 업데이트 (오버플로우 방지)
    while (MAX30102_Read_FIFO(&hmax, &raw_data) == HAL_OK)
    {
    	// memmove를 이용한 고속 시프트
    	memmove(&g_red_buffer[0], &g_red_buffer[1], (WINDOW_SIZE - 1) * sizeof(uint32_t));
    	memmove(&g_ir_buffer[0], &g_ir_buffer[1], (WINDOW_SIZE - 1) * sizeof(uint32_t));

        // 가장 최신 데이터를 윈도우의 맨 끝(가장 우측)에 적재
        g_red_buffer[WINDOW_SIZE - 1] = raw_data.red;
        g_ir_buffer[WINDOW_SIZE - 1] = raw_data.ir;

        // 초반 부팅 시 데이터가 200개 쌓일 때까지 카운트 파악
        if (g_filled_samples < WINDOW_SIZE)
		{
			g_filled_samples++;
		}
    }

    // 2. 4초에 한 번씩 심박수/산소포화도 출력
    g_tick_counter++;
	if (g_tick_counter >= TICK_INTERVAL_4S)
	{
		g_tick_counter = 0;

		// 데이터가 최소한 4초 분량(200샘플)은 꽉 차야 알고리즘이 정상 작동함
		if (g_filled_samples < WINDOW_SIZE)
		{
			printf("[BIO] Buffering data... (%d/%d)\r\n", g_filled_samples, WINDOW_SIZE);
			return;
		}

		float heart_rate = 0.0f;
		float spo2 = 0.0f;

		// 3. 필터 및 DSP 연산 함수 호출하여 심박수/산소포화도 추출
		Calculate_Bio_Metrics(&heart_rate, &spo2);

		// 4. 최종 연산 결과 출력 (유효한 데이터일 때만 출력)
		if (heart_rate > 0.0f)
		{
			printf("[BIO DATA] Heart Rate: %.1f BPM | SpO2: %.1f %%\r\n", heart_rate, spo2);
		}
		else
		{
			printf("[BIO DATA] Finger undetected or stabilizing...\r\n");
		}
	}
}

static void Calculate_Bio_Metrics(float *out_hr, float *out_spo2)
{
	uint64_t red_sum = 0, ir_sum = 0;

	// -------------------------------------------------------------------------
	// 단계 1: DC (평균) 성분 계산
	// -------------------------------------------------------------------------
	for (int i = 0; i < WINDOW_SIZE; i++)
	{
		red_sum += g_red_buffer[i];
		ir_sum += g_ir_buffer[i];
	}
	float red_dc = (float)red_sum / WINDOW_SIZE;
	float ir_dc = (float)ir_sum / WINDOW_SIZE;

	// -------------------------------------------------------------------------
	// 단계 2: [SpO2 개선] MAD (평균 절대 편차) 방식을 이용한 AC 성분 추출
	// 단순 Max-Min 방식은 호흡이나 움직임으로 인한 베이스라인 드리프트에 취약하므로,
	// 신호의 신뢰 가능한 실제 동적 변동 폭(AC)을 잡기 위해 MAD 알고리즘 적용
	// -------------------------------------------------------------------------
	float red_mad = 0.0f;
	float ir_mad = 0.0f;
	for (int i = 0; i < WINDOW_SIZE; i++)
	{
		red_mad += fabsf((float)g_red_buffer[i] - red_dc);
		ir_mad += fabsf((float)g_ir_buffer[i] - ir_dc);
	}
	red_mad /= WINDOW_SIZE;
	ir_mad /= WINDOW_SIZE;

	// -------------------------------------------------------------------------
	// 단계 3: 손가락 미부착 및 신호 불안정 예외 처리
	// 변동 폭(AC)이 너무 작거나(공기 중 노이즈), 적외선 DC 값이 터무니없이 낮으면 무효화
	// -------------------------------------------------------------------------
	if (ir_mad < 30.0f || red_mad < 30.0f || ir_dc < 10000.0f)
	{
		*out_hr = 0.0f;
		*out_spo2 = 0.0f;
		return;
	}

	// -------------------------------------------------------------------------
	// 단계 4: 산소포화도(SpO2) 계산 (R-Value 기반 엠피리컬 표준 보정 공식)
	// -------------------------------------------------------------------------
	float R = (red_mad / red_dc) / (ir_mad / ir_dc);
	float spo2_val = 104.0f - (17.0f * R);

	// 인체 생리학적 임계 한계치 제한 적용
	if (spo2_val > 100.0f) spo2_val = 100.0f;
	if (spo2_val < 70.0f)  spo2_val = 70.0f;
	*out_spo2 = spo2_val;

	// -------------------------------------------------------------------------
	// 단계 5: [심박수 개선] 고주파 노이즈 제거를 위한 5점 이동 평균 필터
	// 맥박 피크 오검출을 유발하는 자잘한 톱니 노이즈를 밀어버려 부드러운 곡선으로 만듦
	// -------------------------------------------------------------------------
	float filtered_ir[WINDOW_SIZE];

	// 필터 윈도우 경계면(시작과 끝 2샘플씩)은 원본 데이터로 바이패스 복사
	filtered_ir[0] = (float)g_ir_buffer[0];
	filtered_ir[1] = (float)g_ir_buffer[1];
	filtered_ir[WINDOW_SIZE - 2] = (float)g_ir_buffer[WINDOW_SIZE - 2];
	filtered_ir[WINDOW_SIZE - 1] = (float)g_ir_buffer[WINDOW_SIZE - 1];

	// 중심부 데이터에 대칭형 5점 평활화(Smoothing) 필터 적용
	for (int i = 2; i < WINDOW_SIZE - 2; i++)
	{
		filtered_ir[i] = (g_ir_buffer[i - 2] + g_ir_buffer[i - 1] + g_ir_buffer[i] +
						  g_ir_buffer[i + 1] + g_ir_buffer[i + 2]) / 5.0f;
	}

	// -------------------------------------------------------------------------
	// 단계 6: 필터링된 깨끗한 신호 기반 시간축 피크 검출
	// -------------------------------------------------------------------------
	int peak_indices[20];
	int peak_count = 0;

	// 50Hz ODR 기준 최대 심박수 한계선(180BPM) 설정을 위한 데드 타임(최소 피크 간격) 방어벽
	// 50Hz / (180BPM / 60초) = 16.6샘플 -> 안전 마진을 두어 최소 15샘플 공백 유지
	const int min_peak_distance = 15;

	for (int i = 1; i < WINDOW_SIZE - 1 && peak_count < 20; i++)
	{
		// 필터링된 부드러운 곡선 위에서 로컬 맥박 피크(극대점)를 정밀하게 서칭
		if (filtered_ir[i] > ir_dc && filtered_ir[i] > filtered_ir[i - 1] && filtered_ir[i] > filtered_ir[i + 1])
		{
			// 첫 피크이거나, 이전 피크로부터 최소 거리(15샘플) 이상 떨어져 있을 때만 진짜 맥박으로 인정
			if (peak_count == 0 || (i - peak_indices[peak_count - 1]) > min_peak_distance)
			{
				peak_indices[peak_count++] = i;
			}
		}
	}

	// -------------------------------------------------------------------------
	// 단계 7: 피크 간의 평균 간격을 기반으로 심박수(BPM) 최종 환산
	// -------------------------------------------------------------------------
	if (peak_count >= 2)
	{
		float total_intervals = 0.0f;
		for (int i = 1; i < peak_count; i++)
		{
			total_intervals += (float)(peak_indices[i] - peak_indices[i - 1]);
		}
		float avg_interval_samples = total_intervals / (float)(peak_count - 1);

		// 50Hz ODR 기준 샘플 수를 분당 박동수(BPM)로 최종 환산 (60초 * 50Hz = 3000)
		*out_hr = 3000.0f / avg_interval_samples;

		// 정상적인 인간 생체 심박 범위를 벗어나면 에러(0.0) 처리
		if (*out_hr < 40.0f || *out_hr > 200.0f) *out_hr = 0.0f;
	}
	else
	{
		*out_hr = 0.0f; // 피크 부족으로 연산 불가(손가락 움직임 등) 처리
	}
}
