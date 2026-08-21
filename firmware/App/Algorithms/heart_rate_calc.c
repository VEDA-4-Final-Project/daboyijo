/**
  ******************************************************************************
  * @file    heart_rate_calc.c
  * @brief   MAX30102 PPG 블록 처리 — 심박수 / SpO2 / 착용 판정
  ******************************************************************************
  *
  * ┌─ 신호 사슬 ────────────────────────────────────────────────────────┐
  * │                                                                    │
  * │  MAX30102 (200Hz 샘플 → 4개 평균 → 50Hz, FIFO 25개마다 INT)        │
  * │      ↓ I2C DMA 150바이트 (0.5초 블록)                              │
  * │  [1] 착용 상태 판정        update_wear_state()                     │
  * │      ↓                                                             │
  * │  [2] 모션 블랭킹 / SpO2 정지 판정                                  │
  * │      ↓                                                             │
  * │  [3] 샘플 루프  ── DC 제거(2차 고역통과) → 7탭 중심 이동평균       │
  * │      │              → 링버퍼 2개에 적재                            │
  * │      │                 · s_ppg_buffer_ir[100]  (2초, 피크용)       │
  * │      │                 · s_ac_hist[800]        (16초, 자기상관용)  │
  * │      └─→ detect_peak()  샘플마다                                   │
  * │      ↓                                                             │
  * │  [4] update_peak_threshold()  창 통계 → 문턱 + 품질 게이트          │
  * │  [5] update_autocorr()        16초 창 → 주기 추정                   │
  * │  [6] 확정 판정 / 노화 / 만료                                        │
  * │                                                                    │
  * └────────────────────────────────────────────────────────────────────┘
  *
  * ── 심박을 얻는 두 경로 ─────────────────────────────────────────────
  *
  *   A. 시간영역 피크 (강신호용, 빠름)
  *      봉우리를 하나씩 찾아 간격을 잰다. 5관문(불응기·진폭·모양·생체대역·
  *      간격일관성)을 통과한 박동 5회로 확정, 약 5초.
  *      단독 확정은 std ≥ PEAK_CONFIRM_STD 인 강신호에서만 허용한다.
  *
  *   B. 자기상관 (약신호용, 느림)
  *      최대 16초 창에서 지연별 상관계수를 훑어 주기를 찾는다. 개별 봉우리가
  *      잡음에 묻혀도 주기는 드러난다. 8초분이 쌓이면 추정을 시작하고,
  *      r ≥ 0.60 이 4블록 연속 일치하면 확정한다.
  *
  *   손가락은 관류가 좋아(std 250~800) A 로 충분하다. 손목은 신호가 10~50배
  *   약해(std 10~50) 개별 봉우리를 못 찾으므로 B 가 그 구간을 담당한다.
  *
  * ── 착용 판정 ───────────────────────────────────────────────────────
  *
  *   두 단계로 나뉜다.
  *     s_is_worn        광학적 접촉. DC 크기와 반사율로 판정한다.
  *     s_wear_confirmed 맥박까지 확인된 착용. 낙상 감지를 여는 근거다.
  *
  *   책상·바닥도 빛을 반사해 s_is_worn 은 참이 될 수 있다. 그러나 진폭 계열
  *   지표(DC·PI·std)는 무기물과 손목이 서로 겹쳐 구분에 쓸 수 없다.
  *   잡음은 진폭은 만들어도 '주기'는 만들지 못하므로, 약신호 구간의
  *   확정 권한은 자기상관 상관계수(r)에만 준다.
  *
  * ── SpO2 가 심박보다 까다로운 이유 ──────────────────────────────────
  *
  *   심박은 '주기'만 맞으면 되지만 SpO2 는 두 파장의 AC '진폭 비율'이라
  *   광 경로가 조금만 흔들려도 값이 통째로 틀어진다. 그래서
  *     · 완전 정지(연속 1.5초)에서만 갱신
  *     · 자기상관 경로로는 계산 불가 (주기만 알고 진폭은 모름)
  *     · 15초 갱신이 없으면 '측정불가(0)'로 내림
  *
  ******************************************************************************
  */
#include "heart_rate_calc.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

/* -----------------------------------------------------------------
 * [시스템 상수 및 알고리즘 튜닝 파라미터]
 *
 * ⚠ SAMPLING_FREQ 는 반드시 MAX30102 의 실효 FIFO 출력 레이트와 같아야 한다.
 *   드라이버 헤더의 값을 직접 물어 두 곳이 따로 놀 수 없게 만든다.
 * ----------------------------------------------------------------- */
#define SAMPLING_FREQ              MAX30102_SAMPLE_RATE_HZ   // 50Hz
#define PRINT_INTERVAL_SAMPLES     SAMPLING_FREQ             // 1초

/* 탈착 기준선.
 * IR의 DC값이 PPG_AIR_THRESHOLD 이하인 상태로 WEAR_DEBOUNCE_SAMPLES만큼 유지되면 탈착 확정 */
#define PPG_AIR_THRESHOLD          35000UL
#define WEAR_DEBOUNCE_SAMPLES      50       // 탈착 확정 가드 타임 (1초)

/* 반사율 하한 (IR DC ÷ LED 전류).
 *
 * DC 절대값만으로는 피부와 반사면을 가를 수 없다. 책상·바닥도 반사광만으로
 * 공기 문턱을 가볍게 넘기 때문이다. 전류로 나눈 반사율을 쓰면 갈린다 —
 * 살아있는 조직이 무기물보다 훨씬 잘 투과·산란시키고, 이 비율은 AGC 가
 * 전류를 어디에 두든 유지된다.
 *   손목 약 2000~3500   /   책상·바닥 약 700~1500 */
#define PPG_MIN_REFLECTANCE        1200UL

/* RED/IR DC 비율은 판정에 쓰지 않는다 —— 진단 표시 전용이다.
 *
 * 조직이 660nm(RED)를 헤모글로빈으로 흡수하므로 무기물과 갈릴 것으로 보고
 * 판정에 넣었다가 뺐다. 실측 분포가 겹친다:
 *   바닥  0.027 ~ 0.047,  0.431 ~ 0.441,  0.730 ~ 0.789
 *   손목  0.669 ~ 0.724
 * 손목이 바닥 범위 안에 완전히 들어가 어떤 문턱으로도 분리되지 않는다.
 * 바닥재의 재질 편차가 조직과 무기물의 차이보다 크다.
 *
 * 광학적 지표로 재질을 맞히려는 시도는 여기서 끝낸다. 대상이 조직인지는
 * '맥박이 있는가'로만 판단한다 —— 확정 게이트(r)와 PULSE_SEARCH_TIMEOUT 이
 * 그 역할을 한다. */

/* 접촉 대상 변화 판정.
 * IR 평균이 한 블록 만에 IR_JUMP_PERCENT 이상 변하면 닿아 있는 물체가 바뀐 것으로 본다.
 * 정상 착용 중 호흡·미세 움직임에 의한 변동은 수 % 수준이라 충분히 구분된다. */
#define IR_JUMP_PERCENT            20U
#define IR_JUMP_SUPPRESS_BLOCKS    3U       // AGC 이득 변경 후 이만큼은 급변 감지를 쉰다

/* 안정화 구간. */
#define SETTLE_SAMPLES             75   // 착용 직후 (1.5초)
#define SETTLE_SAMPLES_GAIN        25   // AGC 이득 변경 후 (0.5초)

/* DC 추정 속도. 안정화 구간에서만 10배로 올려 계단 점프를 즉시 흡수한다. */
#define DC_ALPHA_SETTLE            0.30f    // 착용 직후 과도 응답 빠른 수렴
#define DC_ALPHA_NORMAL            0.03f    // 차단 0.24Hz — 맥동 보존

/* 모션 기준은 IMU 블록의 SVM 표준편차(g)다. */
#define MOTION_BLANKING_THRESHOLD_G 0.05f
#define BLANKING_WINDOW_SAMPLES    50       // 모션 감지 후 동결 시간 (1초)

#define SAMPLE_BUFFER_SIZE         100      // 파형 분석용 로컬 버퍼 크기

/* 불응기: 중복맥파 절흔(dicrotic notch) 방지 기간 */
#define MIN_PEAK_DISTANCE          18   // 초기 불응기: 18샘플 = 360ms → 상한 166bpm.
#define ADAPTIVE_REFRACTORY_RATIO  0.60f// 적응형 불응기: 직전 박동 간격의 60%

/* 연속 기각이 이만큼 쌓이면 기준 간격을 버리고 다시 잡는다.
 * 심박이 실제로 크게 변했거나 기준이 오염됐을 때의 탈출구다. */
#define REACQUIRE_AFTER_REJECTS    3

/* 신호 품질 하한 (절대값) —— 이 값 아래에서는 피크 검출을 아예 하지 않는다. */
#define MIN_PULSE_STD              25.0f

/* 신호 품질 상한 (비율) —— 너무 큰 것도 맥박이 아니다. */
#define MAX_VALID_PI               5.0f

/* 적응형 피크 임계 비율 */
#define PEAK_STD_RATIO             0.60f    // 문턱 = 평균 + 0.6 × 표준편차

/* 시간영역 피크 경로가 '단독으로' 확정할 수 있는 신호 세기.
 *
 * std ≥ 200 : 피크 경로 단독 확정 허용 — 빠르다(약 5초)
 * 그 미만    : 자기상관 동의 필수 — 느리지만(약 20초) 안전하다 */
#define PEAK_CONFIRM_STD           200.0f

/* 간격 검증은 절대 샘플 수가 아니라 최근 평균 대비 상대 오차로 본다. */
#define INTERVAL_TOLERANCE         0.25f    // [상대 오차] 최근 평균 간격 대비 ±25% 범위 내 피크만 진짜로 인정
#define INTERVAL_EMA_ALPHA         0.30f    // [갱신 속도] 검증 통과 시 신규 간격을 30% 가중치로 평균 기준선에 반영

/* 생체 록(Lock) 진입에 필요한 연속 검증 스텝. */
#define STABLE_PEAK_REQUIRED       5

/* 확정 해제 문턱 */
#define VALID_EXIT_COUNT           3

/* 진척도 노화 —— 박동이 이 시간 안에 안 오면 카운터를 깎는다. */
#define STALE_PEAK_SAMPLES         125  // 125샘플 = 2.5초

/* 착용 확정 취소 —— 맥박이 이 시간 동안 사라지면 확정을 거둔다. */
#define WEAR_REVOKE_SAMPLES        1500 // 1500샘플 = 30초

/* 맥박 탐색 포기 —— 광학 접촉은 있는데 이 시간 동안 맥박을 못 찾으면
 * 조직이 아니라고 보고 미착용으로 강등한다.
 *
 * 접촉 판정(반사율·적외비)을 통과했지만 실제로는 맥박이 없는 대상이
 * 존재한다. 그대로 두면 LED 를 계속 태우면서 진단 로그만 무한히 찍는다.
 *
 * 강등 후에는 실제로 떼어낼 때까지(허공 수준으로 DC 가 떨어질 때까지)
 * 재진입을 막는다. 그러지 않으면 같은 대상에서 감지-강등을 반복한다.
 *
 * ⚠ 이 시간은 '최악의 착용 확정 시간'보다 길어야 한다. 접촉이 좋으면
 *   10~15초, 나쁘면 25초 부근에서 확정된다.
 *
 *   30초는 여유가 없었다. 실측 로그에서 손목을 올려둔 채 26초간 정상 신호
 *   (반사율 3300, 적외비 0.915, PI 0.24%)가 나오는데도 확정 직전에
 *   타임아웃이 먼저 걸려 '조직이 아님' 으로 강등됐다. 자기상관 경로는
 *   버퍼 8초 + 연속 일치 2초가 최소이고, 그 앞에 착용 계단 안정화와 AGC
 *   수렴이 붙으면 20초를 쉽게 넘긴다. 60초로 늘려 확정 기회를 준다.
 *
 *   늘린 만큼 무기물에 LED 를 태우는 시간도 늘어나지만, 무기물은 어차피
 *   r 게이트에서 걸리므로 확정되지는 않는다. */
#define PULSE_SEARCH_TIMEOUT       3000 // 3000샘플 = 60초

/* SpO2 유효기간. */
#define SPO2_STALE_SAMPLES         750  // 750샘플 = 15초

/* SpO2 전용 신호 품질 하한.
 *
 * 심박은 '주기'만 맞으면 되지만 SpO2 는 두 파장의 '진폭 비율'이라 요구
 * 수준이 조금 더 높다. 그래서 피크 검출 문턱(MIN_PULSE_STD=25)보다 위에 둔다.
 *
 * ⚠ 손목의 실제 운용 범위를 넘겨 잡으면 안 된다. 실측 std 는 대부분
 *   20~40 이고 40 을 넘는 구간은 드물다. 문턱을 40 으로 두었더니
 *   SpO2 가 아예 나오지 않았다.
 *
 * 이상치 방어의 주력은 이 문턱이 아니라 창 RMS(노이즈 평균화)와
 * 중앙값(이상치 제거)이다. 여기는 가장 나쁜 구간만 걸러내면 된다.
 * 값이 여전히 드물면 25 까지 낮춰도 된다 —— 그러면 피크 게이트와
 * 같아져 사실상 이 조건이 없는 것과 같다. */
#define SPO2_MIN_STD               30.0f

/* R 값 표본 수 —— 이만큼 모아 중앙값을 취한다.
 * 홀수여야 중앙값이 한 개로 정해진다. 5 개면 이상치 2 개까지 견디면서
 * 채우는 시간이 짧다 (9 개는 품질 게이트와 겹쳐 사실상 채워지지 않았다). */
#define SPO2_MEDIAN_N              5

/* =================================================================
 * [1] 자기상관 기반 심박 추정 (손목 약신호 전용 경로)
 * ================================================================= */

/* 분석 창 길이: 800샘플 (16초)
 * - 창이 길수록 백색잡음이 상쇄되어 약한 손목 파형의 주기성 복원에 유리 */
#define AUTOCORR_N                 800

/* 추정을 시작할 최소 축적량: 400샘플 (8초)
 *
 * 창이 가득 차기를 기다리지 않고 이만큼만 쌓이면 추정을 시작한다.
 * 8초면 45BPM 에서도 6주기가 들어와 상관계수가 안정된다.
 * 짧은 창은 r 이 다소 낙관적으로 나오지만, 확정은 r 단독이 아니라
 * AUTOCORR_AGREE_REQUIRED 연속 일치까지 요구하므로 방어선은 유지된다. */
#define AUTOCORR_MIN_N             400

/* 탐색 LAG 범위 (40 ~ 176 BPM 대역) */
#define AUTOCORR_LAG_MIN           17       // 176 BPM 대응 (최소 지연)
#define AUTOCORR_LAG_MAX           75       // 40 BPM 대응 (최대 지연)

/* 상관계수(r) 최소 문턱값 —— 무생물 오확정을 막는 주 게이트.
 *
 * ⚠ 이 값은 update_autocorr() 의 추세 제거 도입과 함께 0.60 에서 내렸다.
 *
 * 이전 구현은 창의 평균·추세를 빼지 않아 r 이 지연과 무관하게 0.97 부근에
 * 붙어 있었다. 즉 0.60 은 아무것도 거르지 못하는 장식이었고, 실제 차단은
 * 엉뚱하게도 '최대점이 경계에 붙었다' 는 기각이 대신하고 있었다.
 * (그래서 손목을 올려도 26초 내내 r 0.00 이었다)
 *
 * 추세를 제거하면 r 이 진짜 피어슨 상관계수가 되어 스케일이 통째로 바뀐다.
 * 재현 시뮬레이션 기준:
 *     무기물(잡음)        0.08
 *     손목 약신호         0.18 ~ 0.42
 *     손가락 강신호       0.86
 * 0.35 는 무기물과 넉넉히 떨어지면서 손목 상위 구간을 살리는 지점이다.
 *
 * 이 값만으로 확정되지 않는다 —— BPM 범위·정지 상태·4블록 연속 일치가
 * 함께 걸린다. 그래도 무기물 오확정이 보이면 여기부터 0.45 로 올릴 것.
 * 반대로 손목이 계속 안 잡히면 로그의 r 실측치를 보고 조금씩 내린다. */
#define AUTOCORR_MIN_R             0.35f

/* 자기상관 경로 최소 진폭 하한선 (0 나눗셈 방지용)
 * - r ≥ 0.60 이 잡음 게이트 역할을 하므로 피크 경로(25.0f)보다 하한선이 낮음 */
#define AUTOCORR_MIN_STD           8.0f

/* 생리학적 심박 범위 (45 ~ 150 BPM).
 * 피크 경로의 간격 타당성 검사가 이 값을 쓴다 — 여기를 좁히면 실제로
 * 빠른 심박을 가진 사람의 박동이 통째로 기각되므로 건드리지 않는다. */
#define AUTOCORR_BPM_MIN           45U
#define AUTOCORR_BPM_MAX           150U

/* 자기상관 경로가 '착용 확정'까지 낼 수 있는 상한 (120 BPM).
 *
 * 생리학적 범위(150)와 일부러 분리한다. 확정은 낙상 감지를 여는 안전 판정이라
 * 단순 추정보다 보수적이어야 하고, 무기물 진동이 만드는 가짜 주기는 지연(lag)
 * 하한 쪽 — 즉 고BPM 대역 — 에 몰리기 때문이다.
 *
 * 120 BPM 은 lag 25 에 해당한다. 실제 손목 확정값은 lag 32~54 구간이라
 * 아티팩트 대역과 넓게 떨어져 있다.
 *
 * 이 상한을 넘는 진짜 심박은 진폭이 커서 피크 경로로 확정되므로 손실은 작다. */
#define AUTOCORR_CONFIRM_BPM_MAX   120U

/* 옥타브(2배수/절반배수) 오류 보정 계수
 * - 절반 주기의 상관계수가 92% 이상으로 대등할 때만 참주기로 채택 */
#define AUTOCORR_OCTAVE_R          0.92f

/* 연속 블록 간 BPM 일치 오차(±10%) 및 잠금(Lock) 조건 (4블록 = 2초)
 * - ±10% 이내 동일 수치가 2초 이상 연속 관측되어야 최종 심박수로 확정 */
#define AUTOCORR_BPM_TOLERANCE     10U
#define AUTOCORR_AGREE_REQUIRED    4

/* --- 교차검증 확정 (두 경로가 서로를 보증하는 빠른 길) ---
 *
 * 피크 경로와 자기상관은 독립적이다. 하나는 시간영역의 봉우리 간격을,
 * 다른 하나는 주파수영역의 주기성을 본다. 둘이 같은 심박을 가리키면
 * 각자의 단독 확정 문턱(피크 std 200 / 자기상관 r 0.60)보다 느슨한
 * 조건으로도 확정할 수 있다.
 *
 * 무기물 오확정에는 오히려 강하다 —— 잡음이 만드는 가짜 주기는 두
 * 추정기에서 서로 다른 값으로 나오기 때문이다. */
#define CROSS_CONFIRM_MIN_R        0.25f   // 피크 5/5 가 보증하므로 단독(0.35)보다 낮다
#define CROSS_CONFIRM_BPM_TOL      20U     // 두 추정기의 BPM 허용 오차(%)

/* =================================================================
 * [2] SpO2 전용 모션 제어
 * ================================================================= */

/* SpO2 움직임 차단 임계값 (가속도 G) 및 정지 유지 조건
 * - SpO2는 AC/DC 비율에 민감하므로 '완전 정지 상태(0.008G)' 1.5초 유지 시에만 갱신 */
#define SPO2_MOTION_LIMIT_G        0.008f
#define SPO2_STILL_REQUIRED        3

/* =================================================================
 * [3] 영위상(Zero-Phase) 저역통과필터 (LPF)
 * ================================================================= */

/* LPF 탭 수 (7-tap 중심 이동평균)
 * - 위상 지연이 0이 되도록 처리하여 맥박 피크 위치 밀림을 방지 (RR 간격 정확도 보존) */
#define LPF_TAPS                   7

/* -----------------------------------------------------------------
 * [모듈 내부 정적(static) 컨텍스트]
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
static uint8_t  s_wear_confirmed = 0;   // 맥박으로 확인된 착용 — 낙상 감지의 근거
static uint32_t s_no_pulse_counter = 0; // 확정 후 맥박이 사라진 누적 시간
static uint32_t s_search_counter = 0;   // 확정 전 맥박을 찾고 있는 누적 시간
static uint8_t  s_search_gave_up = 0;   // 탐색 포기 후 재진입 잠금 (떼어내야 풀린다)
static uint16_t s_air_release_counter = 0; // 잠금 해제용 허공 지속 시간
static uint8_t  s_stable_peak_counter = 0;

/* 시계열 피크 분석 엔진 */
static uint32_t s_last_peak_time = 0;
static uint32_t s_sample_tick = 0;
static uint32_t s_last_accept_tick = 0;  // 마지막으로 '수용된' 박동 시각 (노화 판정용)
static uint32_t s_last_spo2_tick = 0;    // 마지막 SpO2 갱신 시각 (신선도 판정용)

/* 적응형 판정용 추정치 */
static float s_ac_amplitude = 0.0f;   // 최근 2초 창의 peak-to-peak (표시용)
static float s_ac_std = 0.0f;         // 최근 2초 창의 표준편차 (품질 판정 기준)
static float s_peak_threshold = 1e30f;
static float s_interval_avg = 0.0f;   // 최근 박동 간격 평균 (샘플 단위)
static uint8_t s_consec_rejects = 0;  // 연속 기각 수 — 기준 재획득 트리거
static uint16_t s_settle_counter = 0; // 착용 직후 과도 구간 스킵
static uint8_t  s_led_current_seen = 0; // 직전에 관측한 LED 전류 (이득 변경 비율 산출용)

/* 진단용 — 동기화가 왜 안 되는지 눈으로 보기 위한 관측값 */
static uint32_t s_ir_dc_display = 0;   // 직전 블록 IR 평균 (접촉/포화 판단)
static float    s_red_ir_ratio = 0.0f; // 직전 블록 RED/IR DC 비율 (재질 판별용 진단값)
static uint32_t s_prev_ir_mean = 0;    // 접촉 대상 변화 감지용 (직전 블록과 비교)
static uint8_t  s_ir_jump_suppress = 0;  // AGC 직후 급변 감지 억제 블록 수
static uint32_t s_last_inst_bpm = 0;
static uint16_t s_peak_events = 0;
static uint16_t s_peak_rejects = 0;

/* DC 추출 및 중심 이동평균 링버퍼 */
static float s_dc_est_ir = 0.0f;
static float s_dc_est_red = 0.0f;

/* 고역통과 2단째 상태.
 *
 * 1단(EMA)만으로는 옥타브당 6dB 라 손목의 베이스라인 드리프트를 못 이긴다.
 * 손목은 맥박 진폭보다 드리프트가 10배 이상 크기 때문이다.
 * 같은 차단주파수로 한 단을 더 종속해 12dB/옥타브를 만든다.
 * 맥박 대역(0.67~3Hz)의 감쇠는 40BPM 에서도 약 11% 에 그친다. */
static float s_hp2_ir = 0.0f;
static float s_hp2_red = 0.0f;
static float s_ac_ring_ir[LPF_TAPS] = {0.0f};
static float s_ac_ring_red[LPF_TAPS] = {0.0f};
static uint8_t s_ac_idx = 0;
static uint8_t s_ac_filled = 0;

/* 자기상관용 장기 파형 버퍼 (8초) */
static float    s_ac_hist[AUTOCORR_N] = {0.0f};
static uint16_t s_ac_hist_idx = 0;
static uint32_t s_ac_hist_filled = 0;
static float    s_autocorr_r = 0.0f;     // 최대 상관계수 (0~1) — 주기성의 강도
static uint32_t s_autocorr_bpm = 0;      // 자기상관이 추정한 심박수
static uint32_t s_prev_autocorr_bpm = 0; // 직전 블록 추정치 (연속 일치 확인용)
static uint8_t  s_autocorr_agree = 0;    // 연속 일치 횟수

/* SpO2 정지 판정 */
static float    s_block_motion_g = 0.0f; // 이번 블록의 IMU 움직임
static uint8_t  s_still_blocks = 0;      // 연속 정지 블록 수

/* 필터링된 파형 링버퍼.
 * RED 는 SpO2 의 AC 진폭을 RMS 로 재기 위해 IR 과 나란히 보관한다. */
static float s_ppg_buffer_ir[SAMPLE_BUFFER_SIZE] = {0.0f};
static float s_ppg_buffer_red[SAMPLE_BUFFER_SIZE] = {0.0f};
static uint16_t s_buf_idx = 0;
static uint32_t s_buf_filled = 0;
static float s_ac_std_red = 0.0f;      // 창 RMS (RED) — SpO2 전용

/* SpO2 R 값 표본 —— 중앙값을 취해 이상치 한둘에 흔들리지 않게 한다. */
static float   s_spo2_samples[SPO2_MEDIAN_N] = {0.0f};
static uint8_t s_spo2_idx = 0;
static uint8_t s_spo2_filled = 0;

/**
  * @brief 최근 2초 파형의 peak-to-peak 로 피크 문턱을 산출한다.
  *
  * 창(window) 통계는 매 블록 처음부터 다시 계산되므로 구조적으로 잠기지 않는다.
  * 신호가 작아지면 문턱도 따라 내려온다.
  */
static void update_peak_threshold(void)
{
    uint16_t n = (s_buf_filled < SAMPLE_BUFFER_SIZE) ? (uint16_t)s_buf_filled : SAMPLE_BUFFER_SIZE;

    if (n < 10) {
        s_ac_amplitude = 0.0f;
        s_ac_std = 0.0f;
        s_ac_std_red = 0.0f;
        s_peak_threshold = 1e30f;   // 판단 근거가 없으면 검출을 막아둔다
        return;
    }

    float mx = -1e30f, mn = 1e30f;
    float sum = 0.0f, sum_sq = 0.0f;
    float sum_r = 0.0f, sum_sq_r = 0.0f;

    for (uint16_t i = 0; i < n; i++)
    {
        float v = s_ppg_buffer_ir[i];
        if (v > mx) mx = v;
        if (v < mn) mn = v;
        sum    += v;
        sum_sq += v * v;

        /* RED 는 SpO2 의 AC 진폭용으로만 쓴다 (피크 판정에는 관여하지 않는다) */
        float w = s_ppg_buffer_red[i];
        sum_r    += w;
        sum_sq_r += w * w;
    }

    /* RED 창 RMS. IR 과 같은 필터·같은 창을 거치므로 두 값의 비율에서
     * 필터 이득이 상쇄된다 — R 계산에 그대로 쓸 수 있다. */
    float mean_r = sum_r / (float)n;
    float var_r  = (sum_sq_r / (float)n) - (mean_r * mean_r);
    if (var_r < 0.0f) var_r = 0.0f;
    s_ac_std_red = sqrtf(var_r);

    /* peak-to-peak 는 PI 표시용으로만 쓴다 (이상치에 취약해 판정 근거로는 부적합) */
    s_ac_amplitude = mx - mn;

    float mean = sum / (float)n;
    float var  = (sum_sq / (float)n) - (mean * mean);
    if (var < 0.0f) var = 0.0f;              // 부동소수 오차 방어
    float sd = sqrtf(var);
    s_ac_std = sd;                           // 진단 표시용 (품질 판정의 실제 기준값)

    if (sd < MIN_PULSE_STD)
    {
        s_peak_threshold = 1e30f;   // 맥동이라 부를 크기가 아니다
        return;
    }

    /* 상한 검사 —— 생리학적으로 불가능한 크기는 과도 응답이다.
     * 두 경로(피크·자기상관)가 모두 이 문턱을 통해 신호를 받으므로
     * 여기서 막으면 한 곳에서 전부 차단된다. */
    if (s_ir_dc_display > 0 &&
        (s_ac_amplitude * 100.0f / (float)s_ir_dc_display) > MAX_VALID_PI)
    {
        s_peak_threshold = 1e30f;
        return;
    }

    s_peak_threshold = mean + sd * PEAK_STD_RATIO;
}

/**
  * @brief 자기상관으로 심박 주기를 추정한다 (블록당 1회).
  *
  * 정규화 자기상관:  r(τ) = Σ x[i]·x[i+τ] / Σ x[i]²
  * τ 를 40~180BPM 에 해당하는 범위에서 훑어 최대점을 찾는다.
  *
  * 연산량은 60 lag × 약 325 곱 = 약 2만 MAC 이고 0.5초에 한 번만 돈다.
  * 96MHz FPU 코어에서는 무시할 수준이다.
  */
static void update_autocorr(void)
{
    if (s_ac_hist_filled < AUTOCORR_MIN_N)
    {
        s_autocorr_r = 0.0f;
        s_autocorr_bpm = 0;
        return;
    }

    /* 아직 안 찬 버퍼는 링이 아니라 선형이다 — 가장 오래된 샘플이 0번에 있고
     * s_ac_hist_idx 는 아직 한 바퀴를 돌지 않았다. 다 찬 뒤에야 idx 가
     * '가장 오래된 위치' 를 가리킨다. 시작점을 그에 맞춰 고른다. */
    uint16_t n_hist = (s_ac_hist_filled >= AUTOCORR_N)
                    ? AUTOCORR_N : (uint16_t)s_ac_hist_filled;
    uint16_t base   = (s_ac_hist_filled >= AUTOCORR_N) ? s_ac_hist_idx : 0;

    /* 링버퍼를 시간순으로 펴서 읽기 위한 인덱스 변환 */
    #define HIST(i)  s_ac_hist[(base + (i)) % AUTOCORR_N]

    /* 창의 평균과 선형 추세를 제거한다 —— 이 단계가 없으면 자기상관이 무너진다.
     *
     * 고역통과 2단을 거쳐도 8~16초 창에는 저주파 잔여분이 남는다. 차단이
     * 0.24Hz 인데 관측된 베이스라인 드리프트는 0.05Hz 대라 4% 가 통과하고,
     * 손목은 맥동 자체가 작아 그 4% 가 맥동과 같은 크기가 된다.
     * (실측 로그: 창 평균이 -80 인데 표준편차가 50 —— 평균이 신호보다 크다)
     *
     * 그 상태로 r = Σxy/√(Σx²Σy²) 를 구하면 두 가지가 동시에 망가진다.
     *
     *   1) 평균 m 이 남아 있으면 r ≈ (m² + cov)/(m² + var) 가 되어 모든 지연에서
     *      r 이 1 에 붙는다. 실측 재현 시 지연과 무관하게 0.97~0.98 이 나왔다.
     *      즉 AUTOCORR_MIN_R(주기성 게이트)이 사실상 무력화된다.
     *
     *   2) 단조 증감하는 추세는 자기상관도 지연에 대해 단조 감소시킨다.
     *      그래서 최대점이 항상 탐색 범위의 왼쪽 끝(AUTOCORR_LAG_MIN)에 붙고,
     *      아래 경계 기각에 걸려 r 이 0 으로 떨어진다.
     *      실측 로그에서 손목 착용 26초 내내 'r 0.00@0' 이던 원인이 이것이다.
     *
     * 평균과 1차 추세를 빼면 r 이 진짜 피어슨 상관계수가 되어, 무기물(잡음)은
     * 0.1 아래로 내려가고 맥박은 지연 축에서 뚜렷한 봉우리로 드러난다.
     *
     * 인덱스를 창 중앙 기준으로 옮겨 Σt = 0 을 만든다. 그러면 기울기가
     * Σ(t·y)/Σt² 로 곧장 나와 큰 수끼리 빼는 자리 손실이 생기지 않는다. */
    float t_mid  = 0.5f * (float)(n_hist - 1);
    float sum_y  = 0.0f;
    float sum_ty = 0.0f;
    for (uint16_t i = 0; i < n_hist; i++)
    {
        float v = HIST(i);
        sum_y  += v;
        sum_ty += ((float)i - t_mid) * v;
    }

    float win_mean = sum_y / (float)n_hist;

    /* Σt² = n(n²−1)/12  (t 가 창 중앙 기준일 때의 닫힌 형태) */
    float nf      = (float)n_hist;
    float sum_tt  = nf * (nf * nf - 1.0f) / 12.0f;
    float slope   = (sum_tt > 1.0f) ? (sum_ty / sum_tt) : 0.0f;

    /* 추세 제거 후의 값. 아래 상관 계산은 전부 이것만 쓴다. */
    #define HISTD(i)  (HIST(i) - win_mean - slope * ((float)(i) - t_mid))

    /* 무신호 조기 탈출. 아래 지연별 정규화가 각자 에너지를 다시 구하므로
     * 여기서는 '연산할 가치가 있는가' 만 본다. */
    float energy = 0.0f;
    for (uint16_t i = 0; i < n_hist; i++)
    {
        float v = HISTD(i);
        energy += v * v;
    }
    if (energy < 1.0f) { s_autocorr_r = 0.0f; s_autocorr_bpm = 0; return; }

    /* 지연별 상관계수를 모두 보관한다 — 최대점 주변 값을 써서 보간하기 위함 */
    static float r_tab[AUTOCORR_LAG_MAX - AUTOCORR_LAG_MIN + 1];

    float    best_r = 0.0f;
    uint16_t best_lag = 0;

    for (uint16_t lag = AUTOCORR_LAG_MIN; lag <= AUTOCORR_LAG_MAX; lag++)
    {
        /* 정규화 상호상관 —— r = Σxy / √(Σx²·Σy²)
         *
         * 겹치는 두 구간의 에너지를 각각 구해 나눈다. 전체 에너지를 공용
         * 분모로 쓰면 지연이 길수록 분모가 작아져 낮은 BPM 쪽으로 편향된다. */
        float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
        uint16_t n = n_hist - lag;

        for (uint16_t i = 0; i < n; i++)
        {
            float x = HISTD(i);
            float y = HISTD(i + lag);
            sxy += x * y;
            sxx += x * x;
            syy += y * y;
        }

        float denom = sqrtf(sxx * syy);
        float r = (denom >= 1.0f) ? (sxy / denom) : 0.0f;

        r_tab[lag - AUTOCORR_LAG_MIN] = r;
        if (r > best_r) { best_r = r; best_lag = lag; }
    }

    /* 부(sub)조화 오인 보정.
     *
     * 주기 T 인 신호는 2T 에서도 상관이 높다. 최대점이 2T 에 잡히면 심박이
     * 절반으로 보고되므로 절반 지연의 상관도 확인한다.
     *
     * PPG 는 중복맥파 절흔 때문에 T/2 성분이 원래 어느 정도 존재한다.
     * 그래서 절반 쪽이 '거의 대등할 때'(AUTOCORR_OCTAVE_R)만 채택한다 —
     * 문턱이 낮으면 반대로 정상 심박을 2배로 보고하게 된다. */
    if (best_lag >= AUTOCORR_LAG_MIN * 2)
    {
        uint16_t half = best_lag / 2;
        float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
        uint16_t n = n_hist - half;

        for (uint16_t i = 0; i < n; i++)
        {
            float x = HISTD(i);
            float y = HISTD(i + half);
            sxy += x * y;
            sxx += x * x;
            syy += y * y;
        }

        float denom = sqrtf(sxx * syy);
        if (denom >= 1.0f)
        {
            float r_half = sxy / denom;
            if (r_half > best_r * AUTOCORR_OCTAVE_R)
            {
                best_lag = half;
                best_r = r_half;
            }
        }
    }

    #undef HISTD
    #undef HIST

    /* 경계 최대점은 신뢰하지 않는다.
     *
     * 최대점이 탐색 범위의 끝에 붙는다는 건 진짜 봉우리가 범위 밖에 있는데
     * 경계에서 잘렸다는 뜻이다. 심박이 아니라 고정 주파수 아티팩트다.
     * 아래 포물선 보간이 좌우 이웃을 참조하므로 배열 경계 보호도 겸한다. */
    if (best_lag <= AUTOCORR_LAG_MIN || best_lag >= AUTOCORR_LAG_MAX)
    {
        s_autocorr_r = 0.0f;
        s_autocorr_bpm = 0;
        return;
    }

    /* 포물선 보간 —— 지연의 소수점 자리를 복원한다.
     *
     * 지연은 정수 샘플이라 BPM 해상도가 심박수에 따라 크게 달라진다.
     *   60BPM (지연 50) → 1샘플 = 1.2BPM
     *  120BPM (지연 25) → 1샘플 = 4.8BPM   ← 창을 늘려도 이건 안 줄어든다
     *
     * 최대점과 좌우 이웃 세 점에 포물선을 맞춰 꼭짓점을 구하면
     * 지연을 소수점까지 얻을 수 있어 해상도가 1BPM 아래로 내려간다. */
    float lag_f = (float)best_lag;

    uint16_t idx = best_lag - AUTOCORR_LAG_MIN;
    float r_m = r_tab[idx - 1];
    float r_0 = r_tab[idx];
    float r_p = r_tab[idx + 1];

    float denom_i = (r_m - 2.0f * r_0 + r_p);
    if (denom_i < -1e-6f)   /* 위로 볼록해야 꼭짓점이 최대점이다 */
    {
        float delta = 0.5f * (r_m - r_p) / denom_i;
        if (delta > -1.0f && delta < 1.0f) lag_f += delta;
    }

    s_autocorr_r = best_r;
    s_autocorr_bpm = (uint32_t)((60.0f * (float)SAMPLING_FREQ / lag_f) + 0.5f);
}

/**
  * @brief 피크 기각 공통 처리 (범위 밖 / 간격 불일치 두 경로가 공유)
  *
  * 고립된 기각 하나로는 검증 진척도를 깎지 않고, 2회 연속일 때만 깎는다.
  * 정상 PPG 도 박동 하나쯤은 어긋나므로 매번 깎으면 카운터가 오르내리기만
  * 반복해 확정에 도달하지 못한다. 리듬이 아예 없는 잡음은 연속 기각이
  * 계속 나오므로 이 완화로도 걸러진다.
  */
static void on_peak_rejected(void)
{
    s_peak_rejects++;
    s_consec_rejects++;

    if (s_consec_rejects >= 2 && s_stable_peak_counter > 0)
    {
        s_stable_peak_counter--;
    }

    if (s_hr_state == HR_STAT_VALID && s_stable_peak_counter < VALID_EXIT_COUNT)
    {
        s_hr_state = HR_STAT_ACQUIRING;
    }

    /* 연속 기각이 쌓이면 기준 간격을 버리고 다시 잡는다.
     * 심박이 실제로 크게 변했거나 기준이 오염된 경우의 탈출구다. */
    if (s_consec_rejects >= REACQUIRE_AFTER_REJECTS)
    {
        s_interval_avg = 0.0f;
        s_consec_rejects = 0;
    }
}

/**
  * @brief 현재 관류지수(%) — 신호가 생체 신호인지 판별하는 유일한 근거
  */
static float current_pi(void)
{
    if (s_ir_dc_display == 0) return 0.0f;
    return s_ac_amplitude * 100.0f / (float)s_ir_dc_display;
}

/* SpO2 R 표본을 비운다.
 *
 * 닿아 있는 대상이 바뀌었거나 움직임으로 신호가 끊겼을 때만 부른다.
 * AGC 이득 변경에서는 부르지 않는다 —— R 은 비율이라 전류가 바뀌어도
 * 값이 유효하고, 버리면 9박동을 다시 모아야 한다. */
static void reset_spo2_samples(void)
{
    s_spo2_idx = 0;
    s_spo2_filled = 0;
}

/**
  * @brief 신호 처리 파이프라인을 완전히 비우고 안정화 구간을 건다.
  *
  * 닿아 있는 대상이 바뀌었을 때(착용 감지 / 접촉 변화 / 전체 리셋) 쓴다.
  * 필터 상태와 파형 이력을 한곳에서 모두 지워 이전 대상의 잔재가 남지
  * 않게 한다 — 서로 다른 물체의 파형이 한 창에 섞이면 그 혼합물에서
  * 가짜 주기성이 나온다.
  *
  * 이득 변경은 대상이 그대로이므로 이 함수가 아니라
  * HeartRateCalc_NotifyGainChange() 의 스케일 보정을 쓴다.
  *
  * @param settle 안정화 구간 길이(샘플). 계단이 클수록 길게 준다.
  */
static void reset_signal_pipeline(uint16_t settle)
{
    s_dc_est_ir  = 0.0f;   // 0 이면 다음 샘플로 새로 씨앗내린다
    s_dc_est_red = 0.0f;
    s_hp2_ir     = 0.0f;
    s_hp2_red    = 0.0f;

    s_ac_idx = 0;
    s_ac_filled = 0;
    s_buf_idx = 0;
    s_buf_filled = 0;

    s_last_peak_time = 0;      // 다음 피크를 새 기준점으로 삼는다
    s_ac_amplitude = 0.0f;
    s_peak_threshold = 1e30f;  // 창 통계가 쌓이기 전에는 검출을 막아둔다
    s_settle_counter = settle;

    /* 자기상관 이력과 누적된 일치 횟수까지 함께 비운다 */
    s_ac_hist_idx = 0;
    s_ac_hist_filled = 0;
    s_autocorr_r = 0.0f;
    s_autocorr_bpm = 0;
    s_prev_autocorr_bpm = 0;
    s_autocorr_agree = 0;

    /* 대상이 바뀌었으므로 이전 대상에서 모은 R 표본도 버린다 */
    reset_spo2_samples();
}

void HeartRateCalc_Init(void)
{
    HeartRateCalc_Reset();
    s_wear_confirmed = 0;
    s_no_pulse_counter = 0;
    printf("[ HEALTH ] module initialized. (%dHz, %d samples/block)\r\n",
           SAMPLING_FREQ, MAX30102_BLOCK_SAMPLES);
}

/**
  * @brief 착용 상태 머신. 블록당 1회만 평가한다.
  * @return 신호 처리를 계속할지 여부 (0 이면 이번 블록은 건너뛴다)
  */
static uint8_t update_wear_state(uint32_t ir_mean, uint16_t count)
{
    /* 접촉 없음 판정은 두 가지다.
     *   허공   DC 가 아예 낮다
     *   반사면 DC 는 나오는데 전류 대비 반사율이 조직 수준에 못 미친다
     *
     * 둘 다 '빛이 새는가'를 볼 뿐, 닿아 있는 것이 조직인지는 못 가른다.
     * 바닥에 밀착시키면 무기물도 조직만큼 반사해 여기를 통과한다.
     * 그 경우는 PULSE_SEARCH_TIMEOUT 이 맥박 유무로 걸러낸다. */
    uint8_t  led = MAX30102_GetLedCurrent();
    uint8_t  low_reflectance = 0;

    /* 다음 이득 변경 때 '변경 전 전류'로 쓰인다. 이 함수는 AutoGain 과
     * NotifyGainChange 다음에 돌므로 여기 값은 항상 현재 적용된 전류다. */
    s_led_current_seen = led;

    if (led > 0 && (ir_mean / led) < PPG_MIN_REFLECTANCE)
    {
        low_reflectance = 1;
    }

    /* 탐색 포기 잠금 해제 —— 허공이 '일정 시간 지속'되어야 한다.
     *
     * 반사율 판정만으로는 풀지 않는다. 그건 대상에 닿은 채로도 성립할 수
     * 있어서 같은 대상에서 감지-강등이 반복된다.
     *
     * 단발 블록으로도 풀면 안 된다. 포기 직후 AutoGain 이 전류를 기본값으로
     * 되돌리며 FIFO 를 비우는데, 그 과도 구간에 DC 가 순간적으로 낮게 읽히면
     * 떼어내지 않았는데도 잠금이 풀린다. 탈착 판정과 같은 디바운스를 건다. */
    if (ir_mean < PPG_AIR_THRESHOLD)
    {
        if (s_air_release_counter < WEAR_DEBOUNCE_SAMPLES)
        {
            s_air_release_counter += count;
        }
        if (s_air_release_counter >= WEAR_DEBOUNCE_SAMPLES)
        {
            s_search_gave_up = 0;
        }
    }
    else
    {
        s_air_release_counter = 0;
    }

    if (ir_mean < PPG_AIR_THRESHOLD || low_reflectance)
    {
        s_stable_peak_counter = 0;

        if (s_is_worn == 1)
        {
            /* 실제 수신 샘플 수로 센다. 블록이 짧게 올 때 MAX30102_BLOCK_SAMPLES
             * 를 쓰면 디바운스 시간이 실제보다 빠르게 흐른다. */
            s_air_counter += count;
            if (s_air_counter >= WEAR_DEBOUNCE_SAMPLES)
            {
                s_is_worn = 0;
                s_hr_state = HR_STAT_NONE;
                s_air_counter = 0;

                /* 물리적 탈착 — 착용 확정을 여기서만 취소한다.
                 * 낙상 감지가 꺼지는 유일한 경로다. */
                if (s_wear_confirmed)
                {
                    s_wear_confirmed = 0;
                    printf("[ WEAR ] 탈착 — 착용 확정 해제, 낙상 감지 중지\r\n");
                }
                else if (low_reflectance)
                {
                    /* 실측 반사율을 함께 찍어 문턱이 적절한지 판단할 수 있게 한다 */
                    printf("[ WEAR ] 탈착 상태 진입 (반사면 — 반사율 %lu/%lu, LED 0x%02X)\r\n",
                           (unsigned long)(ir_mean / (led ? led : 1)),
                           (unsigned long)PPG_MIN_REFLECTANCE, led);
                }
                else
                {
                    printf("[ WEAR ] 탈착 상태 진입 (허공)\r\n");
                }
            }
        }
    }
    else
    {
        s_air_counter = 0;

        /* 접촉 대상이 바뀌었는지 감지한다.
         *
         * 책상 → 손목처럼 접촉이 끊기지 않고 물체만 바뀌면 s_is_worn 이 1 을
         * 유지해 아래 착용 진입 블록이 실행되지 않는다. 그러면 이전 물체의
         * DC 추정이 남아 가짜 AC 가 만들어지고 맥박으로 오인될 수 있다.
         * IR 평균이 한 블록 만에 크게 뛰면 다른 것에 닿았다고 보고 새로 시작한다.
         *
         * 단, AGC 이득 변경 직후에는 쉰다. 전류를 바꾸면 DC 가 뛰는 것이
         * 당연한데 이 감지기는 그것과 실제 접촉 변화를 구분하지 못한다.
         * (이득 변경에 필요한 처리는 NotifyGainChange() 가 이미 수행한다) */
        if (s_ir_jump_suppress > 0)
        {
            s_ir_jump_suppress--;
        }
        else if (s_is_worn == 1 && s_prev_ir_mean > 0)
        {
            uint32_t hi = (ir_mean > s_prev_ir_mean) ? ir_mean : s_prev_ir_mean;
            uint32_t lo = (ir_mean > s_prev_ir_mean) ? s_prev_ir_mean : ir_mean;

            if (((hi - lo) * 100U / hi) >= IR_JUMP_PERCENT)
            {
                reset_signal_pipeline(SETTLE_SAMPLES);
                s_interval_avg = 0.0f;
                s_consec_rejects = 0;
                s_stable_peak_counter = 0;
                s_hr_state = HR_STAT_ACQUIRING;

                /* 착용 확정도 함께 거둔다.
                 *
                 * 접촉 대상이 바뀌었다는 것은 지금 무엇에 닿아 있는지 모른다는
                 * 뜻이다. 이전 물체에서 얻은 확정을 넘겨받으면, 손목에서 책상으로
                 * 옮겨도 취소 타이머가 만료될 때까지 책상이 낙상 감지 대상이 된다. */
                if (s_wear_confirmed)
                {
                    s_wear_confirmed = 0;
                    s_no_pulse_counter = 0;
                    s_current_bpm = 0;
                    s_current_spo2 = 0;
                }

                s_search_counter = 0;   // 대상이 바뀌었으니 탐색 시간도 새로 센다

                printf("[ WEAR ] 접촉 대상 변화 감지 (IR %lu → %lu) — 재탐색\r\n",
                       (unsigned long)s_prev_ir_mean, (unsigned long)ir_mean);
            }
        }

        /* 맥박 탐색 타임아웃.
         *
         * 접촉 판정은 통과했는데 제한 시간 안에 맥박을 찾지 못하면 조직이
         * 아니라고 보고 미착용으로 내린다. LED 를 끄고 로그도 멈춘다.
         * 재진입은 실제로 떼어낼 때까지(위 허공 판정) 막는다. */
        if (s_is_worn == 1 && !s_wear_confirmed)
        {
            s_search_counter += count;

            if (s_search_counter >= PULSE_SEARCH_TIMEOUT)
            {
                s_is_worn = 0;
                s_hr_state = HR_STAT_NONE;
                s_search_counter = 0;
                s_air_counter = 0;
                s_search_gave_up = 1;

                printf("[ WEAR ] %d초간 맥박 없음 — 조직이 아닌 것으로 판단, 탐색 중지 "
                       "(적외비 %.3f, 반사율 %lu)\r\n",
                       PULSE_SEARCH_TIMEOUT / SAMPLING_FREQ,
                       (double)s_red_ir_ratio,
                       (unsigned long)(ir_mean / (led ? led : 1)));
            }
        }
        else
        {
            s_search_counter = 0;   // 확정됐으면 탐색 타이머는 의미가 없다
        }

        /* 탐색을 포기한 대상에는 다시 들어가지 않는다 (떼어내야 풀린다) */
        if (s_is_worn == 0 && !s_search_gave_up)
        {
            s_is_worn = 1;
            s_hr_state = HR_STAT_ACQUIRING;
            s_stable_peak_counter = 0;
            s_search_counter = 0;

            /* 착용 순간 IR 이 공기(~1만) → 조직(~18만) 으로 계단 점프한다.
             * 긴 안정화 구간을 주고, 간격 기준까지 새로 잡는다. */
            reset_signal_pipeline(SETTLE_SAMPLES);
            s_interval_avg = 0.0f;
            s_consec_rejects = 0;

            /* 이어지는 몇 블록은 급변 감지를 쉰다.
             *
             * 계단 점프는 이 블록에서 끝나지 않는다. 조직에 눌리면서 DC 가
             * 몇 초에 걸쳐 계속 내려앉는데, 급변 감지기는 그 과도응답과
             * 실제 접촉 대상 변화를 구분하지 못한다. 쉬게 하지 않으면
             * 착용 직후 '대상 변화'가 오탐으로 떠서 방금 건 안정화와
             * 자기상관 이력이 곧바로 다시 지워진다. */
            s_ir_jump_suppress = IR_JUMP_SUPPRESS_BLOCKS;

            printf("[ WEAR ] 착용 감지 (생체 신호 탐색 시작)\r\n");
        }
    }

    s_prev_ir_mean = ir_mean;

    /* 하강 에지에서 내부 메모리를 즉시 비워 잡음 오염을 막는다. */
    if (s_prev_worn == 1 && s_is_worn == 0) HeartRateCalc_Reset();
    s_prev_worn = s_is_worn;

    return s_is_worn;
}

/**
  * @brief 필터링이 끝난 샘플 1개에 대한 피크 검출 및 BPM/SpO2 갱신
  *
  * 5점 비교법. 블록 덕분에 뒤쪽 2샘플을 확보할 수 있어 3점 비교보다
  * 잔떨림에 훨씬 강하다.
  */
static void detect_peak(void)
{
    if (s_buf_filled < 5) return;

    /* 최신 샘플이 s_buf_idx-1 이므로, 후보는 2칸 뒤 = 앞뒤 2개씩을 갖춘 지점 */
    uint16_t i_n2 = (uint16_t)((s_buf_idx + SAMPLE_BUFFER_SIZE - 1) % SAMPLE_BUFFER_SIZE);
    uint16_t i_n1 = (uint16_t)((s_buf_idx + SAMPLE_BUFFER_SIZE - 2) % SAMPLE_BUFFER_SIZE);
    uint16_t i_c  = (uint16_t)((s_buf_idx + SAMPLE_BUFFER_SIZE - 3) % SAMPLE_BUFFER_SIZE);
    uint16_t i_p1 = (uint16_t)((s_buf_idx + SAMPLE_BUFFER_SIZE - 4) % SAMPLE_BUFFER_SIZE);
    uint16_t i_p2 = (uint16_t)((s_buf_idx + SAMPLE_BUFFER_SIZE - 5) % SAMPLE_BUFFER_SIZE);

    float c = s_ppg_buffer_ir[i_c];

    /* 불응기 = 절대 하한과 '직전 간격의 60%' 중 큰 값 */
    uint32_t refractory = MIN_PEAK_DISTANCE;
    if (s_interval_avg > 0.0f)
    {
        uint32_t adaptive = (uint32_t)(s_interval_avg * ADAPTIVE_REFRACTORY_RATIO);
        if (adaptive > refractory) refractory = adaptive;
    }
    if ((s_sample_tick - s_last_peak_time) < refractory) return;

    /* 적응형 진폭 문턱 (블록마다 창 통계로 갱신됨) */
    if (c <= s_peak_threshold) return;

    /* 앞뒤 2샘플 범위의 최대점이면 피크로 인정한다 (5샘플 창).
     *
     * 단조 증가 → 단조 감소까지는 요구하지 않는다. 손목은 SNR 이 2:1 수준이라
     * 봉우리 꼭대기의 잔떨림 하나로도 단조성이 깨져 진짜 박동이 탈락한다.
     * 창 크기는 그대로라 잡음 억제력은 유지된다. */
    if (c <= s_ppg_buffer_ir[i_n1] || c <= s_ppg_buffer_ir[i_n2]) return;
    if (c <= s_ppg_buffer_ir[i_p1] || c <= s_ppg_buffer_ir[i_p2]) return;

    uint32_t interval = s_sample_tick - s_last_peak_time;

    s_peak_events++;

    if (s_last_peak_time == 0)
    {
        /* 첫 피크는 기준점으로만 삼는다 (간격을 잴 상대가 없다).
         * 검증 진척도와 간격 기준은 건드리지 않는다 — 필요한 초기화는
         * 이미 착용 감지 경로에서 끝나 있다. */
        s_last_peak_time = s_sample_tick;
        return;
    }

    uint32_t inst_bpm = (60UL * SAMPLING_FREQ) / interval;
    s_last_inst_bpm = inst_bpm;

    if (inst_bpm < AUTOCORR_BPM_MIN || inst_bpm > AUTOCORR_BPM_MAX)
    {
        /* 생체 범위 밖 = 피크를 하나 놓쳤거나 잡음을 하나 더 물었다는 뜻.
         *
         * 간격이 너무 짧으면(=상한 초과) 절흔 같은 이중 검출이므로 기준점을
         * 옮기지 않는다. 옮기면 곧 도착할 진짜 박동까지 간격이 어긋난다.
         * 간격이 너무 길면 박동을 놓친 것이라 기준점을 새로 잡는다. */
        if (inst_bpm <= AUTOCORR_BPM_MAX) s_last_peak_time = s_sample_tick;

        /* SpO2 진폭 트래커는 건드리지 않는다 — 기각은 심장 주기의 경계가
         * 아니므로, 여기서 리셋하면 다음 박동이 반쪽 진폭만 보게 된다. */
        on_peak_rejected();
        return;
    }

    /* 최근 평균 간격 대비 상대 편차로 판정한다 */
    uint8_t accepted;
    if (s_interval_avg <= 0.0f)
    {
        accepted = 1;                       // 비교 기준이 없으니 이번 값을 기준으로 채택
        s_interval_avg = (float)interval;
    }
    else
    {
        float dev = fabsf((float)interval - s_interval_avg) / s_interval_avg;
        accepted = (dev <= INTERVAL_TOLERANCE) ? 1 : 0;
    }

    if (accepted)
    {
        s_last_peak_time = s_sample_tick;
        s_last_accept_tick = s_sample_tick;   // 노화 타이머 갱신
        s_consec_rejects = 0;
        s_interval_avg = s_interval_avg * (1.0f - INTERVAL_EMA_ALPHA)
                       + (float)interval * INTERVAL_EMA_ALPHA;

        /* --- Ratio-of-Ratios SpO2 ---
         *
         * AC 진폭은 한 주기의 peak-to-peak 이 아니라 창 전체의 RMS 를 쓴다.
         * p2p 는 노이즈 스파이크 하나가 값을 통째로 정해버리는데, 손목은
         * 맥동과 센서 잡음이 같은 크기라 그 영향이 지배적이다. RMS 는 창
         * 전체를 평균해 이상치 영향이 √N 만큼 줄어든다.
         * R 은 비율이라 p2p 든 RMS 든 스케일 상수가 상쇄되어 그대로 쓴다.
         *
         * 세 관문을 모두 넘어야 표본으로 채택한다.
         *   정지     움직이는 중의 진폭 비율은 신뢰할 수 없다
         *   신호 세기 SpO2 는 심박보다 높은 품질을 요구한다 (SPO2_MIN_STD)
         *   생리 범위 벗어난 값은 잘라 보고하지 않고 아예 버린다 */
        if (s_still_blocks >= SPO2_STILL_REQUIRED &&
            s_ac_std     >= SPO2_MIN_STD &&
            s_ac_std_red >= 1.0f &&
            s_dc_est_ir > 1000.0f && s_dc_est_red > 1000.0f)
        {
            float r = (s_ac_std_red / s_dc_est_red) / (s_ac_std / s_dc_est_ir);
            float inst_spo2 = 104.0f - 17.0f * r;

            if (inst_spo2 >= 70.0f && inst_spo2 <= 100.0f)
            {
                /* 표본을 모아 중앙값을 취한다.
                 *
                 * 예전에는 박동마다 EMA(가중 5%)로 섞었는데, 정수 연산이라
                 * 값이 위로 올라가지 못했다 —— 92%에서 1 올리려면 새 값이
                 * 112% 여야 했다. 반면 내려가는 건 쉬워서, 한 번 낮게 잡히면
                 * 영영 회복되지 않았다.
                 *
                 * 중앙값은 그 비대칭이 없고 이상치 한둘에도 흔들리지 않는다. */
                s_spo2_samples[s_spo2_idx] = inst_spo2;
                s_spo2_idx = (uint8_t)((s_spo2_idx + 1) % SPO2_MEDIAN_N);
                if (s_spo2_filled < SPO2_MEDIAN_N) s_spo2_filled++;

                if (s_spo2_filled >= SPO2_MEDIAN_N)
                {
                    /* 표본이 9개뿐이라 삽입정렬 사본으로 충분하다 */
                    float sorted[SPO2_MEDIAN_N];
                    for (uint8_t i = 0; i < SPO2_MEDIAN_N; i++) sorted[i] = s_spo2_samples[i];

                    for (uint8_t i = 1; i < SPO2_MEDIAN_N; i++)
                    {
                        float key = sorted[i];
                        int8_t j = (int8_t)i - 1;
                        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
                        sorted[j + 1] = key;
                    }

                    s_current_spo2 = (uint32_t)(sorted[SPO2_MEDIAN_N / 2] + 0.5f);
                    s_last_spo2_tick = s_sample_tick;
                }
            }
        }

        if (s_hr_state == HR_STAT_ACQUIRING)
        {
            if (s_stable_peak_counter < STABLE_PEAK_REQUIRED)
            {
                s_stable_peak_counter++;
            }

            /* 약신호에서는 피크 경로 단독으로 확정하지 않는다.
             *
             * 무기물 위에서도 잡음 피크 몇 개가 우연히 등간격이 되는 일이
             * 드물지 않다. 신호가 손가락급으로 강할 때(PEAK_CONFIRM_STD)만
             * 시간영역 단독 판단을 신뢰하고, 그 아래는 자기상관에 맡긴다. */
            if (s_stable_peak_counter >= STABLE_PEAK_REQUIRED &&
                s_ac_std >= PEAK_CONFIRM_STD)
            {
                s_hr_state = HR_STAT_VALID;

                /* 맥박 확인은 살아있는 조직 위에 있다는 증거다. 여기서
                 * 착용을 확정하고 물리적으로 뺄 때까지 유지한다. 손목은 검출이
                 * 간헐적이라, 맥박이 잠깐 끊겼다고 낙상 감지를 내리면 안 된다. */
                if (!s_wear_confirmed)
                {
                    s_wear_confirmed = 1;
                    printf("[ WEAR ] 맥박 확인 — 착용 확정, 낙상 감지 활성\r\n");
                }
            }
        }

        if (s_current_bpm == 0 || s_hr_state != HR_STAT_VALID) {
            s_current_bpm = inst_bpm;
        } else {
            s_current_bpm = (s_current_bpm * 7 + inst_bpm * 3) / 10;
        }

    }
    else
    {
        /* 간격이 기준에서 벗어났다.
         *   평균보다 짧다 → 절흔 등 이중 검출. 기준점을 옮기면 진짜 다음 박동
         *                   까지 어긋나므로 그대로 두고 기다린다.
         *   평균보다 길다 → 박동을 놓친 것. 기준점을 다시 잡는다. */
        if ((float)interval > s_interval_avg) s_last_peak_time = s_sample_tick;

        on_peak_rejected();

        /* 기각된 간격으로는 평균을 갱신하지 않는다.
         *
         * 갱신하면 되먹임이 생긴다 — 박동을 놓쳐 간격이 길어지면 평균이 오르고,
         * 평균에 비례하는 불응기가 함께 늘어 다음 진짜 박동까지 잡아먹는다.
         * 연속 기각 시의 기준 재획득은 on_peak_rejected() 가 담당한다. */
    }
}

void HeartRateCalc_ProcessBlock(const MAX30102_Data_t *samples, uint16_t count, float motion_g)
{
    if (samples == NULL || count == 0) return;

    /* -----------------------------------------------------------------
     * [1단계] 블록 평균 기반 착용 상태 판정
     *   샘플마다 흔들리는 판정 대신 블록 평균을 쓴다. 단발 노이즈로
     *   착용/탈착이 튀는 것을 구조적으로 막는다.
     * ----------------------------------------------------------------- */
    uint64_t ir_sum = 0, red_sum = 0;
    for (uint16_t i = 0; i < count; i++)
    {
        ir_sum  += samples[i].ir;
        red_sum += samples[i].red;
    }
    uint32_t ir_mean = (uint32_t)(ir_sum / count);
    s_ir_dc_display = ir_mean;

    /* RED/IR DC 비율 —— 반사량이 아니라 '무엇에 닿아 있는가'를 보는 값.
     *
     * 조직은 660nm(RED)를 헤모글로빈이 강하게 흡수하고 880nm(IR)는 훨씬 잘
     * 통과시키므로 IR DC 가 RED DC 보다 뚜렷하게 크다. 무기물은 두 파장을
     * 비슷하게 반사한다. 반사율(DC ÷ 전류)과 달리 밀착 정도에 영향받지 않아,
     * 바닥에 딱 붙여 반사율이 조직 수준으로 올라간 경우에도 구분이 남는다.
     *
     * 지금은 진단 표시 전용이다. 손목과 무기물의 실측 분포를 확인한 뒤에
     * 판정 문턱을 정한다. */
    s_red_ir_ratio = (ir_mean > 0)
                   ? ((float)(uint32_t)(red_sum / count) / (float)ir_mean)
                   : 0.0f;

    if (!update_wear_state(ir_mean, count)) goto SYSTEM_PRINT_LOOP;

    /* -----------------------------------------------------------------
     * [2단계] IMU 연동 모션 블랭킹
     * ----------------------------------------------------------------- */
    s_block_motion_g = motion_g;

    /* SpO2 전용 정지 판정.
     * 심박은 '주기'만 맞으면 되지만 SpO2 는 두 파장의 AC '크기 비율'을 재므로
     * 광 경로가 조금만 흔들려도 값이 통째로 틀어진다. 그래서 훨씬 엄격하게,
     * 연속 여러 블록 정지가 유지될 때만 갱신을 허용한다. */
    if (motion_g <= SPO2_MOTION_LIMIT_G)
    {
        if (s_still_blocks < SPO2_STILL_REQUIRED) s_still_blocks++;
    }
    else
    {
        s_still_blocks = 0;
    }

    if (motion_g > MOTION_BLANKING_THRESHOLD_G)
    {
        s_blanking_counter = BLANKING_WINDOW_SAMPLES;

        /* 움직임이 신호를 블랭킹할 정도면 SpO2 는 그 순간부터 신뢰할 수 없다.
         * 15초 유효기간을 기다릴 이유가 없다 — 이미 낡은 값임을 알고 있다.
         * (심박은 주기만 보면 되므로 계속 유지한다) */
        s_current_spo2 = 0;
    }

    if (s_blanking_counter > 0)
    {
        s_blanking_counter = (s_blanking_counter > count) ? (s_blanking_counter - count) : 0;
        reset_spo2_samples();   // 움직인 구간의 R 표본은 신뢰할 수 없다
        goto SYSTEM_PRINT_LOOP;
    }

    /* 무기물 판정을 위한 타임아웃은 더 이상 없다.
     * 착용은 맥박 확인으로만 성립하므로(파일 상단 [착용 판정 구조] 참조),
     * 책상·침대는 애초에 확정되지 않는다. 시간을 재며 추측할 이유가 없다. */

    /* -----------------------------------------------------------------
     * [3단계] 배열 전체를 훑는 필터 + 피크 검출 파이프라인
     * ----------------------------------------------------------------- */
    for (uint16_t i = 0; i < count; i++)
    {
        /* --- DC 추출 및 AC 분리 --- */
        if (s_dc_est_ir == 0.0f)  s_dc_est_ir  = (float)samples[i].ir;
        if (s_dc_est_red == 0.0f) s_dc_est_red = (float)samples[i].red;

        /* 정상 계수 0.03(=0.97 유지)은 차단주파수 0.24Hz 로, 베이스라인 흔들림은
         * 걷어내면서 맥동(1~3Hz)은 손상 없이 통과시킨다.
         *
         * 다만 착용 직후에는 이 속도가 너무 느리다. IR 이 1만→18만으로 계단 점프할 때
         * 1.5초 뒤에도 계단의 10%(=1만8천)가 남아 '진폭 21015' 같은 가짜 신호가 된다.
         * 그 5초 남짓한 구간에서 잠금이 성공하느냐가 운에 좌우됐다.
         * 안정화 구간에서만 계수를 10배로 올려 DC 를 즉시 따라붙게 한다. */
        float dc_alpha = (s_settle_counter > 0) ? DC_ALPHA_SETTLE : DC_ALPHA_NORMAL;

        s_dc_est_ir  = (s_dc_est_ir  * (1.0f - dc_alpha)) + ((float)samples[i].ir  * dc_alpha);
        s_dc_est_red = (s_dc_est_red * (1.0f - dc_alpha)) + ((float)samples[i].red * dc_alpha);

        /* 1단: 원신호에서 DC 제거 */
        float ac1_ir  = (float)samples[i].ir  - s_dc_est_ir;
        float ac1_red = (float)samples[i].red - s_dc_est_red;

        /* 2단: 남은 저주파 잔여분을 한 번 더 걷어낸다.
         * 손목처럼 맥박이 드리프트보다 작은 부위에서 이 단이 결정적이다. */
        s_hp2_ir  = (s_hp2_ir  * (1.0f - dc_alpha)) + (ac1_ir  * dc_alpha);
        s_hp2_red = (s_hp2_red * (1.0f - dc_alpha)) + (ac1_red * dc_alpha);

        s_ac_ring_ir[s_ac_idx]  = ac1_ir  - s_hp2_ir;
        s_ac_ring_red[s_ac_idx] = ac1_red - s_hp2_red;
        s_ac_idx = (uint8_t)((s_ac_idx + 1) % LPF_TAPS);
        if (s_ac_filled < LPF_TAPS) s_ac_filled++;

        /* 링이 다 차기 전에는 중심값이 정의되지 않는다. */
        if (s_ac_filled < LPF_TAPS) continue;

        /* --- 중심 이동평균 (대칭 FIR → 위상 지연 0) ---
         * 결과는 LPF_DELAY 만큼 과거 샘플에 대응한다. 모든 피크에 동일하게
         * 적용되는 고정 지연이라 RR 간격 계산에는 영향이 없다. */
        float sum_ir = 0.0f, sum_red = 0.0f;
        for (uint8_t t = 0; t < LPF_TAPS; t++)
        {
            sum_ir  += s_ac_ring_ir[t];
            sum_red += s_ac_ring_red[t];
        }
        float filtered_ir  = sum_ir  / (float)LPF_TAPS;
        float filtered_red = sum_red / (float)LPF_TAPS;

        s_ppg_buffer_ir[s_buf_idx]  = filtered_ir;
        s_ppg_buffer_red[s_buf_idx] = filtered_red;
        s_buf_idx = (uint16_t)((s_buf_idx + 1) % SAMPLE_BUFFER_SIZE);
        if (s_buf_filled < SAMPLE_BUFFER_SIZE) s_buf_filled++;

        s_sample_tick++;

        /* 안정화 구간의 샘플은 자기상관 이력에 넣지 않는다.
         *
         * 이 구간은 DC_ALPHA_SETTLE(0.30)로 도는데, 그러면 고역통과 코너가
         * 약 2.8Hz 로 올라가 맥박 대역(1Hz)이 8배 가까이 감쇠한다. 즉 여기서
         * 나온 파형은 맥박이 거의 지워진 상태다.
         *
         * 그걸 창에 넣으면 400샘플 중 75개가 '주기 없는 구간'이 되어 상관계수를
         * 끌어내린다. 1.5초 늦게 채우더라도 깨끗한 창으로 시작하는 편이
         * 확정까지 더 빠르다. 피크 판정도 같은 이유로 쉰다. */
        if (s_settle_counter > 0)
        {
            s_settle_counter--;
            continue;
        }

        /* 자기상관용 장기 이력 (최대 16초) */
        s_ac_hist[s_ac_hist_idx] = filtered_ir;
        s_ac_hist_idx = (uint16_t)((s_ac_hist_idx + 1) % AUTOCORR_N);
        if (s_ac_hist_filled < AUTOCORR_N) s_ac_hist_filled++;

        detect_peak();
    }

    /* 이번 블록의 파형으로 다음 판정에 쓸 문턱을 갱신한다.
     * 창 전체를 매번 다시 훑기 때문에 잘못된 값에 갇히지 않는다. */
    update_peak_threshold();

    /* -----------------------------------------------------------------
     * 자기상관 경로 —— 손목처럼 개별 피크가 잡음에 묻히는 경우의 대안
     *
     * 시간영역 검출이 확정에 도달하지 못해도, 창에 뚜렷한 주기가 있으면
     * 그건 맥박이다. 잡음은 진폭은 만들어도 주기는 만들지 못한다.
     * ----------------------------------------------------------------- */
    update_autocorr();

    /* 확정 조건. 다섯 가지를 모두 넘어야 한다.
     *
     *   진폭 하한   순수 잡음에서 우연한 주기가 잡히는 것을 막는다.
     *   상관계수    주기성의 강도.
     *   BPM 범위    아티팩트가 몰리는 고BPM 대역을 배제한다.
     *   정지 상태   손동작은 1~2Hz 라 심박 대역과 겹쳐 주기성만으로는 못 가른다.
     *               사람은 정지해도 맥박이 뛰지만 정지한 물체는 주기가 사라진다.
     *   연속 일치   아래에서 확인. 한 번의 r 스파이크로는 확정하지 않는다. */
    uint8_t autocorr_ok = (s_ac_std >= AUTOCORR_MIN_STD) &&
                          (s_autocorr_r >= AUTOCORR_MIN_R) &&
                          (s_autocorr_bpm >= AUTOCORR_BPM_MIN) &&
                          (s_autocorr_bpm <= AUTOCORR_CONFIRM_BPM_MAX) &&
                          (s_still_blocks >= SPO2_STILL_REQUIRED);

    if (autocorr_ok && s_prev_autocorr_bpm > 0)
    {
        uint32_t hi = (s_autocorr_bpm > s_prev_autocorr_bpm) ? s_autocorr_bpm : s_prev_autocorr_bpm;
        uint32_t lo = (s_autocorr_bpm > s_prev_autocorr_bpm) ? s_prev_autocorr_bpm : s_autocorr_bpm;

        if (((hi - lo) * 100U / hi) <= AUTOCORR_BPM_TOLERANCE)
        {
            if (s_autocorr_agree < AUTOCORR_AGREE_REQUIRED) s_autocorr_agree++;
        }
        else
        {
            s_autocorr_agree = 0;
        }
    }
    else
    {
        s_autocorr_agree = 0;
    }
    s_prev_autocorr_bpm = autocorr_ok ? s_autocorr_bpm : 0;

    if (s_autocorr_agree >= AUTOCORR_AGREE_REQUIRED)
    {
        /* 확정 상태에서도 계속 갱신한다. 약신호 구간에서는 시간영역 피크가
         * 막혀 있어 이 경로 말고는 BPM 을 갱신할 수단이 없다. */
        if (s_current_bpm == 0) {
            s_current_bpm = s_autocorr_bpm;
        } else {
            s_current_bpm = (s_current_bpm * 7 + s_autocorr_bpm * 3) / 10;
        }

        if (s_hr_state != HR_STAT_VALID)
        {
            s_hr_state = HR_STAT_VALID;
            s_stable_peak_counter = STABLE_PEAK_REQUIRED;
        }
        s_last_accept_tick = s_sample_tick;   // 노화 타이머 갱신 (주기가 살아있다)

        if (!s_wear_confirmed)
        {
            s_wear_confirmed = 1;
            printf("[ WEAR ] 맥박 확인(주기성 r=%.2f, %lu BPM) — 착용 확정, 낙상 감지 활성\r\n",
                   (double)s_autocorr_r, (unsigned long)s_autocorr_bpm);
        }
    }

    /* -----------------------------------------------------------------
     * 교차검증 확정 —— 피크 경로와 자기상관이 같은 심박을 가리킬 때
     *
     * 피크 경로는 검증을 마쳤지만(5/5) 단독 확정 문턱(std 200)에 못 미치고,
     * 자기상관은 아직 단독 문턱(r 0.60)에 도달하지 못한 구간이 손목에서는
     * 길게 이어진다. 그 사이 두 추정기가 이미 같은 답을 내고 있다면
     * 더 기다릴 이유가 없다.
     *
     * 비교 기준은 s_interval_avg 다 —— 순간값(s_last_inst_bpm)은 박동마다
     * 크게 튀지만 이쪽은 수용된 간격의 이동평균이라 안정적이다.
     * ----------------------------------------------------------------- */
    if (s_hr_state != HR_STAT_VALID &&
        s_stable_peak_counter >= STABLE_PEAK_REQUIRED &&
        s_still_blocks >= SPO2_STILL_REQUIRED &&
        s_ac_std >= MIN_PULSE_STD &&
        s_autocorr_r >= CROSS_CONFIRM_MIN_R &&
        s_autocorr_bpm >= AUTOCORR_BPM_MIN &&
        s_autocorr_bpm <= AUTOCORR_CONFIRM_BPM_MAX &&
        s_interval_avg > 1.0f)
    {
        uint32_t peak_bpm = (uint32_t)((60.0f * (float)SAMPLING_FREQ / s_interval_avg) + 0.5f);
        uint32_t hi = (peak_bpm > s_autocorr_bpm) ? peak_bpm : s_autocorr_bpm;
        uint32_t lo = (peak_bpm > s_autocorr_bpm) ? s_autocorr_bpm : peak_bpm;

        if (hi > 0 && ((hi - lo) * 100U / hi) <= CROSS_CONFIRM_BPM_TOL)
        {
            s_hr_state = HR_STAT_VALID;
            s_last_accept_tick = s_sample_tick;

            if (!s_wear_confirmed)
            {
                s_wear_confirmed = 1;
                printf("[ WEAR ] 맥박 확인(교차검증 피크 %lu / 주기성 %lu BPM, r=%.2f) — 착용 확정, 낙상 감지 활성\r\n",
                       (unsigned long)peak_bpm, (unsigned long)s_autocorr_bpm,
                       (double)s_autocorr_r);
            }
        }
    }

    /* 진척도 노화 —— 박동열이 끊기면 쌓아둔 검증을 되돌린다.
     *
     * 이게 없으면 '연속 5회'가 시간과 무관해진다. 품질 게이트에 막혀 검출이
     * 몇 초씩 끊겨도 카운터가 남아, 흩어진 잡음 피크가 연속된 박동열처럼
     * 누적되기 때문이다.
     *
     * VALID 해제 판단은 PI 가 아니라 이 리듬 단절 여부로만 한다 —
     * PI 로 강등하면 PI 0.05% 대의 정상 약신호가 계속 쫓겨난다. */
    if (s_stable_peak_counter > 0 &&
        (s_sample_tick - s_last_accept_tick) > STALE_PEAK_SAMPLES)
    {
        s_stable_peak_counter--;
        s_last_accept_tick = s_sample_tick;   // 한 번에 하나씩만 깎는다

        if (s_hr_state == HR_STAT_VALID && s_stable_peak_counter < VALID_EXIT_COUNT)
        {
            s_hr_state = HR_STAT_ACQUIRING;
        }
    }

    /* SpO2 신선도 —— 오래되면 값을 내린다.
     *
     * 자기상관으로 심박이 유지되는 동안에도 SpO2 는 갱신되지 않는다(R 을 못 구함).
     * 그 상태로 옛 값을 계속 내보내면 서버는 그것이 현재 값인 줄 안다. */
    if (s_current_spo2 > 0 &&
        (s_sample_tick - s_last_spo2_tick) > SPO2_STALE_SAMPLES)
    {
        s_current_spo2 = 0;
    }

    /* 착용 확정 취소 판정.
     * 맥박이 오래 사라지면 지금 닿아 있는 것이 사람이 아니라고 봐야 한다.
     * 광학 접촉만으로는 책상↔손목 이동을 구분할 수 없기 때문에 필요하다. */
    if (s_wear_confirmed)
    {
        if (s_hr_state == HR_STAT_VALID)
        {
            s_no_pulse_counter = 0;
        }
        else
        {
            s_no_pulse_counter += count;
            if (s_no_pulse_counter >= WEAR_REVOKE_SAMPLES)
            {
                s_wear_confirmed = 0;
                s_no_pulse_counter = 0;
                s_current_bpm = 0;
                s_current_spo2 = 0;
                printf("[ WEAR ] 맥박 %d초간 소실 — 착용 확정 해제, 낙상 감지 중지\r\n",
                       WEAR_REVOKE_SAMPLES / SAMPLING_FREQ);
            }
        }
    }

SYSTEM_PRINT_LOOP:

    /* -----------------------------------------------------------------
     * [4단계] 1초 주기 로깅
     * ----------------------------------------------------------------- */
    s_print_counter = (uint16_t)(s_print_counter + count);
    if (s_print_counter >= PRINT_INTERVAL_SAMPLES)
    {
        s_print_counter = 0;

        if (s_is_worn && s_hr_state == HR_STAT_VALID)
        {
            /* SpO2 가 0 이면 '측정 불가' 를 명시한다. 숫자 0% 로 보이면
             * 산소포화도가 0 이라는 뜻으로 읽혀 오해를 부른다.
             *
             * 확정 후에도 std 와 표본 수를 함께 찍는다. 이게 없으면
             * SpO2 가 안 나올 때 신호가 약한 탓인지 표본이 덜 찬 탓인지
             * 구분할 수 없다. */
            if (s_current_spo2 > 0) {
                printf("[DISPLAY] 심박수: %lu BPM | 산소포화도: %lu%% (std %.0f/%.0f 표본 %u/%d)\r\n",
                       (unsigned long)s_current_bpm, (unsigned long)s_current_spo2,
                       (double)s_ac_std, (double)SPO2_MIN_STD,
                       s_spo2_filled, SPO2_MEDIAN_N);
            } else {
                printf("[DISPLAY] 심박수: %lu BPM | 산소포화도: 측정불가 (std %.0f/%.0f 표본 %u/%d)\r\n",
                       (unsigned long)s_current_bpm,
                       (double)s_ac_std, (double)SPO2_MIN_STD,
                       s_spo2_filled, SPO2_MEDIAN_N);
            }
        }
        else if (s_is_worn && s_hr_state == HR_STAT_ACQUIRING)
        {
            /* 확정이 안 될 때 어느 관문에서 막혔는지 보기 위한 진단 줄이다.
             *
             *   PI      관류지수(AC/DC). 0.2~1% 가 손목 정상 범위,
             *           0.1% 미만이면 접촉 압력이나 LED 전류를 의심한다.
             *   DC      25만 초과면 ADC 포화, 3만 미만이면 접촉 불량.
             *   std/문턱 '피크 0' 이 신호가 없어서인지 문턱이 높아서인지 가른다.
             *   r/정지/일치  자기상관 확정의 세 관문. */
            float pi = current_pi();   /* 표시 전용 — 판정에는 쓰지 않는다 */

            char thr_txt[24];
            if (s_peak_threshold > 1e29f) {
                snprintf(thr_txt, sizeof(thr_txt), "차단");   // 신호가 잡음 수준
            } else {
                snprintf(thr_txt, sizeof(thr_txt), "%.0f", (double)s_peak_threshold);
            }

            /* 반사율(DC÷전류)과 RED/IR 을 함께 찍는다. 앞은 '빛이 새는가',
             * 뒤는 '무엇에 닿아 있는가' 라 서로 다른 것을 본다. */
            uint8_t led_now = MAX30102_GetLedCurrent();
            uint32_t reflect = (led_now > 0) ? (s_ir_dc_display / led_now) : 0;

            printf("[DISPLAY] 동기화 (%d/%d) | BPM %lu | DC %lu | LED 0x%02X | 반사 %lu 적외비 %.3f | PI %.3f%% | std %.0f/%.0f 문턱 %s | r %.2f@%lu 정지 %u/%d 일치 %u/%d | 피크 %u 기각 %u | 착용 %s\r\n",
                   s_stable_peak_counter, STABLE_PEAK_REQUIRED,
                   (unsigned long)s_last_inst_bpm,
                   (unsigned long)s_ir_dc_display,
                   led_now,
                   (unsigned long)reflect, (double)s_red_ir_ratio,
                   (double)pi,
                   (double)s_ac_std, (double)MIN_PULSE_STD, thr_txt,
                   (double)s_autocorr_r, (unsigned long)s_autocorr_bpm,
                   s_still_blocks, SPO2_STILL_REQUIRED,
                   s_autocorr_agree, AUTOCORR_AGREE_REQUIRED,
                   s_peak_events, s_peak_rejects,
                   s_wear_confirmed ? "확정" : "미확정");

            if (s_ir_dc_display > 250000UL) {
                printf("          ⚠ ADC 포화 — MAX30102_LED_CURRENT 를 낮추세요\r\n");
            }
        }
        else
        {
            printf("[DISPLAY] 기기 미착용 (신호 대기 모드)\r\n");
        }

        /* 진단 카운터는 출력 주기마다 비운다 (직전 1초 구간의 통계) */
        s_peak_events = 0;
        s_peak_rejects = 0;
    }
}

void HeartRateCalc_Reset(void)
{
    s_hr_state = HR_STAT_NONE;
    s_is_worn = 0;
    s_current_bpm = 0;
    s_current_spo2 = 0;
    s_blanking_counter = 0;
    s_print_counter = 0;
    s_stable_peak_counter = 0;
    s_search_counter = 0;
    s_search_gave_up = 0;   // 잠금이 남으면 리셋 후 영영 착용을 감지하지 못한다
    s_air_release_counter = 0;

    s_last_peak_time = 0;
    s_last_accept_tick = 0;
    s_last_spo2_tick = 0;
    s_ac_hist_idx = 0;
    s_ac_hist_filled = 0;
    s_autocorr_r = 0.0f;
    s_autocorr_bpm = 0;
    s_prev_autocorr_bpm = 0;
    s_autocorr_agree = 0;
    s_still_blocks = 0;
    s_sample_tick = 0;

    s_ac_amplitude = 0.0f;
    s_interval_avg = 0.0f;
    s_consec_rejects = 0;
    s_last_inst_bpm = 0;
    s_peak_events = 0;
    s_peak_rejects = 0;

    reset_signal_pipeline(0);   // 전체 리셋에서는 안정화 대기가 불필요하다

    for (uint8_t i = 0; i < LPF_TAPS; i++) {
        s_ac_ring_ir[i] = 0.0f;
        s_ac_ring_red[i] = 0.0f;
    }
    for (uint16_t i = 0; i < SAMPLE_BUFFER_SIZE; i++)
    {
        s_ppg_buffer_ir[i]  = 0.0f;
        s_ppg_buffer_red[i] = 0.0f;
    }
    s_ac_std_red = 0.0f;
    reset_spo2_samples();
}

void HeartRateCalc_NotifyGainChange(void)
{
    /* 이득이 바뀌어도 닿아 있는 대상은 그대로다. 신호가 사라진 게 아니라
     * 스케일만 바뀐 것이므로 파형 이력을 버리지 않고 비율로 보정한다.
     * 광전류가 LED 전류에 선형 비례하므로 AC 진폭도 같은 비율로 변한다.
     *
     * 이력을 비우면 자기상관 창을 8초부터 다시 쌓아야 해서, AGC 가 한 번
     * 움직일 때마다 최초 측정이 그만큼 밀린다.
     *
     * 접촉 대상이 바뀐 경우는 reset_signal_pipeline() 로 비우는 것이 맞다 —
     * 그때는 다른 물체의 파형이 섞이는 것이라 스케일 문제가 아니다. */
    uint8_t now_led = MAX30102_GetLedCurrent();

    if (s_led_current_seen == 0 || now_led == 0)
    {
        /* 기준 전류를 모르면 비율을 낼 수 없으므로 통째로 비운다. */
        reset_signal_pipeline(SETTLE_SAMPLES_GAIN);
    }
    else
    {
        float scale = (float)now_led / (float)s_led_current_seen;

        for (uint16_t i = 0; i < AUTOCORR_N; i++)      s_ac_hist[i] *= scale;
        for (uint16_t i = 0; i < SAMPLE_BUFFER_SIZE; i++)
        {
            s_ppg_buffer_ir[i]  *= scale;
            s_ppg_buffer_red[i] *= scale;   // RED 도 같은 비율로 (R 은 그대로 유지된다)
        }
        for (uint8_t i = 0; i < LPF_TAPS; i++)
        {
            s_ac_ring_ir[i]  *= scale;
            s_ac_ring_red[i] *= scale;
        }

        /* DC 추정기는 스케일이 아니라 재씨앗으로 처리한다.
         *
         * 비율 계산은 근사다. 광량이 전류에 정확히 비례하지 않고 FIFO 도
         * 비워지므로 예측 DC 와 실제 DC 사이에 수천 카운트가 남는다.
         * 그 오차는 AC 계단으로 들어가는데, DC_ALPHA_NORMAL(차단 0.24Hz)로는
         * 감쇠에 8초가 걸리고 그동안 자기상관 창이 통째로 오염된다.
         *
         * 0 으로 두면 다음 샘플 값을 그대로 씨앗 삼아(아래 샘플 루프 참조)
         * 오차 없이 시작한다. 남는 불연속은 AC 진폭 한 개 분량뿐이다.
         *
         * AC 이력(s_ac_hist 등)은 위에서 스케일하는 것이 맞다 —— 그쪽은
         * 진폭이 실제로 전류에 비례하고, 버리면 축적 시간을 다시 내야 한다. */
        s_dc_est_ir  = 0.0f;
        s_dc_est_red = 0.0f;
        s_hp2_ir     = 0.0f;
        s_hp2_red    = 0.0f;

        /* R 표본은 버리지 않는다 —— 비율이라 전류가 바뀌어도 유효하다. */

        /* AGC 가 FIFO 를 비우므로 최대 25샘플의 시간 공백이 창 안에
         * 이음매로 남는다. 그 구간만 피크 판정을 쉰다. */
        s_settle_counter = SETTLE_SAMPLES_GAIN;
    }

    s_led_current_seen = now_led;

    /* 이득 변경으로 DC 가 뛰는 동안 IR 급변 감지가 오탐하지 않게 막는다 */
    s_ir_jump_suppress = IR_JUMP_SUPPRESS_BLOCKS;
}

/* 과도응답 안정화 구간인가. AGC 가 이 값을 보고 이득 판단을 미룬다. */
uint8_t HeartRateCalc_IsSettling(void) { return (s_settle_counter > 0); }

/* 낙상 판정이 참조하는 착용 여부.
 * 광학적 접촉(s_is_worn)이 아니라 '맥박으로 확인된 착용'을 돌려준다.
 * 책상·침대는 맥박이 없어 여기서 1 이 될 수 없다. */
uint8_t  HeartRateCalc_IsWorn(void)     { return s_wear_confirmed; }
uint8_t  HeartRateCalc_HasContact(void) { return s_is_worn; }
uint32_t HeartRateCalc_GetBPM(void)  { return (s_hr_state == HR_STAT_VALID) ? s_current_bpm : 0; }
uint32_t HeartRateCalc_GetSpO2(void) { return (s_hr_state == HR_STAT_VALID) ? s_current_spo2 : 0; }
