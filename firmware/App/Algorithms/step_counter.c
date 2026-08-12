/**
  ******************************************************************************
  * @file    step_counter.c
  * @brief   BMI270 가속도 블록 기반 만보기 (손목 착용 기준)
  ******************************************************************************
  *
  * ── 신호 처리 4단계 ─────────────────────────────────────────────────
  *
  *   1단계  중력축 추정  3축을 각각 저역통과해 중력 벡터의 방향을 얻는다
  *   2단계  수직 투영    순간 가속도를 그 축에 투영하고 중력 크기를 뺀다
  *   3단계  평활화       4Hz 저역통과로 손떨림·충격 성분 제거
  *   4단계  피크+리듬    임계 상향 교차를 걸음 후보로 잡고, 규칙적인 것만 인정
  *
  * ── 왜 SVM(벡터 크기)이 아니라 중력축 투영인가 ──────────────────────
  *
  *   처음에는 SVM 을 썼다. 자세와 무관한 스칼라라 손목에 적합해 보였고,
  *   낙상 감지도 같은 양을 쓴다. 바꾼 이유는 수평 흔들림 오검출 때문이다.
  *
  *   SVM 은 수평 성분을 제곱한다. 가속도를 중력 방향 성분 d∥ 와 수직인
  *   성분 d⊥ 로 나누면
  *
  *       SVM = √((1 + d∥)² + |d⊥|²) ≈ 1 + d∥ + |d⊥|²/2
  *
  *   d∥ 는 선형으로 지나가지만 d⊥ 는 제곱되어 들어온다. 진폭이 작을 때는
  *   무시할 수준이지만(0.5g 흔들림 → 약 0.06g) 커지면 급격히 불어난다.
  *   투영은 내적이라 d⊥ 가 아예 0 으로 떨어진다.
  *
  *   수직 운동 없이 순수 수평 흔들림만 준 시뮬레이션에서 나온 가짜 걸음 수
  *   (10초, 정답 0):
  *
  *       수평 0.5g   SVM  0 / 투영  0
  *       수평 1.0g   SVM 40 / 투영  1     ← 여기서 갈린다
  *       수평 1.5g   SVM 40 / 투영 38
  *       수평 2.0g   SVM 59 / 투영 57
  *
  *   ⚠ 1.5g 를 넘으면 중력축 추정 자체가 흔들려 투영도 무너진다.
  *     기기를 격하게 휘두르는 상황까지 막아주지는 못한다.
  *
  * ── 왜 중력 크기를 빼는가 (고정 1.0g 가 아니라) ─────────────────────
  *
  *   가속도계 스케일 오차 때문에 정지 상태에서도 벡터 크기가 1.00 이 아니라
  *   1.03~1.05 로 읽힌다 (fall_detection.c 의 '움직임 지표' 항목 참조).
  *   1.0 을 빼면 그 오차가 상수 오프셋으로 남아 임계값 판정을 통째로 밀어버린다.
  *   추정한 중력 벡터의 크기를 빼면 오차든 자세 변화든 함께 따라 내려간다.
  *
  *   시정수는 약 0.5초(alpha 0.02)다. 걷기 시작 직후 0.5초 동안은 추정값이
  *   아직 따라붙는 중이라 신호가 부풀지만, 아래 리듬 게이트가 어차피 4걸음을
  *   요구하므로 그 구간이 오검출로 새지 않는다.
  *
  * ── 왜 '규칙성' 을 요구하는가 ───────────────────────────────────────
  *
  *   손목은 만보기에 최악의 위치다. 타이핑, 설거지, 문 여닫기, 차량 진동이
  *   전부 1~3Hz 대역에 걸린다. 피크 하나를 곧바로 한 걸음으로 세면 하루에
  *   수천 걸음이 허공에서 생긴다.
  *
  *   그래서 걸음 후보를 즉시 세지 않고 보류한다. 후보가 서로 비슷한 간격으로
  *   4번 연속 들어와야(= 사람이 실제로 걷고 있다는 뜻) 비로소 인정하고,
  *   그때 보류했던 4걸음을 한꺼번에 더한다. 총합이 깎이지 않으면서
  *   단발성 흔들림은 통과하지 못한다.
  *
  *   ⚠ 이 방식으로도 '팔을 규칙적으로 흔드는 동작'(설거지, 양치질)은 걸러지지
  *     않는다. 상용 손목 만보기도 마찬가지다 — 구조적 한계로 받아들이고,
  *     대신 4걸음 미만의 자잘한 오검출을 없애는 데 집중한다.
  *
  * ── 착용 판정을 쓰지 않는 이유 ──────────────────────────────────────
  *
  *   이 모듈은 착용 여부를 묻지 않는다. IMU 는 손에 들고 있든 손목에 차든
  *   똑같이 움직임을 재고, 걸음 수는 그 움직임만으로 성립하기 때문이다.
  *
  *   PPG 접촉 판정(HeartRateCalc_HasContact)을 게이트로 걸면 가방 속 흔들림
  *   같은 오검출을 조금 더 막을 수 있지만, 대가로 MAX30102 가 죽거나 접촉이
  *   불안정한 순간 만보기가 통째로 멈춘다. 서로 다른 센서의 고장이 전파되는
  *   구조라 이득보다 손해가 크다. 가방 속 흔들림은 위의 리듬 게이트가
  *   대부분 걸러내므로 그쪽에 맡긴다.
  *
  *   ⚠ 그 대신 기기를 손에 쥐고 규칙적으로 흔들면 걸음으로 세어진다.
  *     의도된 동작이다 — 책상에서 흔들어보며 튜닝할 수 있다는 뜻이기도 하다.
  *
  * ── 검증 이력 (2026-08) ─────────────────────────────────────────────
  *
  *   정상 속도 보행에서 60걸음 → 63걸음 (1.05배). 케이던스 76~96spm 구간의
  *   간격 수열은 62~89샘플에 고르게 모였고, 20걸음짜리 구간은 irregular /
  *   too-fast 없이 20 을 그대로 세었다.
  *
  *   ⚠ 느린 보행에서는 계수가 배로 뛴다. 약 43spm(걸음당 1.4초)으로 걸었을 때
  *     20걸음에 봉우리가 38개 나왔다 — 한 걸음의 두 국면(체중 이동, 발 딛기)이
  *     0.7초나 벌어져 각각 검출되기 때문이다. 정상 속도에서는 그 간격이 0.25초로
  *     좁아져 4Hz 평활화가 하나로 뭉갠다.
  *
  *     '걸음을 세느라 천천히 걷는' 실측 방식 자체가 이 오차를 만든다. 검증할
  *     때는 반드시 평소 속도로, 방향 전환 없이 긴 직선 구간에서 걸을 것.
  *     아주 느린 보행까지 정확히 세려면 봉우리 진폭 비교 같은 다른 판별이
  *     필요하며, 지금 구조로는 감당하지 못한다.
  *
  * ── 임계값을 만질 때 ────────────────────────────────────────────────
  *
  *   먼저 STEP_DEBUG_INTERVALS 를 1 로 켜고 간격 수열부터 볼 것. 총합만 보고
  *   임계값을 옮기면 원인을 잘못 짚는다 (실제로 그렇게 세 번 헛돌았다).
  *
  *     적게 세면    STEP_PEAK_THRESHOLD_G 를 낮춘다 (0.12 → 0.09)
  *     많이 세면    STEP_PEAK_THRESHOLD_G 를 높인다 (0.12 → 0.15)
  *     간격이 교대  한 걸음이 두 봉우리 → STEP_MIN_INTERVAL 을 짧은 쪽 위로
  *     간격이 균일  신호가 2배 주파수 → 보행 속도부터 확인. 정상 속도인데도
  *                  그렇다면 시간 게이트로는 못 막는다
  *
  *   ⚠ STEP_MIN_INTERVAL 을 30 아래로 내리지 말 것. 실측 로그에서 20~29샘플
  *     짜리 여분 봉우리가 9회 걸러졌다 — 이 게이트가 실제로 일하고 있다.
  *
  ******************************************************************************
  */
#include "step_counter.h"
#include <stdio.h>
#include <math.h>

/* -----------------------------------------------------------------
 * [만보기 튜닝 파라미터] — 전부 BMI270 ODR 100Hz 기준
 * ----------------------------------------------------------------- */

/* 중력·자세 추정 저역통과 (차단 약 0.3Hz).
 * 보행 성분(1~3Hz)은 건드리지 않으면서 자세 변화만 따라간다. */
#define STEP_GRAVITY_ALPHA        0.02f

/* 평활화 저역통과 (차단 약 4Hz).
 * 보행 기본파와 2차 고조파는 남기고 그 위의 노이즈만 자른다. */
#define STEP_SMOOTH_ALPHA         0.25f

/* 피크 판정 히스테리시스.
 * 손목 보행 진폭은 0.2~0.5g 다. 임계 0.12g 는 정지 시 노이즈 바닥
 * (0.001~0.003g)의 40배라 오검출 여유가 충분하다.
 * 한 번 잡으면 RESET 아래로 내려갔다 와야 다시 무장한다 — 임계선 위에서
 * 신호가 잘게 떨릴 때 한 걸음이 여러 번 세지는 것을 막는다. */
#define STEP_PEAK_THRESHOLD_G     0.12f
#define STEP_PEAK_RESET_G         0.05f

/* 시간 게이트 (샘플 수).
 *   MIN 30  = 0.30초 → 200spm 상한. 명백한 진동만 쳐내는 최소한의 방어선이다.
 *   MAX 200 = 2.0초  → 30spm 하한.  이보다 느리면 걷는 중이라고 볼 수 없다. */
#define STEP_MIN_INTERVAL         30
#define STEP_MAX_INTERVAL         200

/* 리듬 게이트.
 *   REQUIRED 4   규칙적으로 4연속이어야 걸음으로 인정 (보류분은 한꺼번에 가산)
 *   TOL_PCT  50  직전 간격 대비 ±50% 안이어야 같은 보행으로 본다.
 *                케이던스는 서서히 변하므로 이 정도면 가감속을 다 담는다. */
#define STEP_RUN_REQUIRED         4
#define STEP_RHYTHM_TOL_PCT       50

/* 이정표 로그 간격.
 * 로그 조건은 블록당 한 번(0.5초)만 검사하므로 아무리 낮춰도 초당 2줄이 상한이다.
 * 실측·튜닝 중에는 30 정도로 촘촘히 보고, 안정되면 100 이상으로 올리면 된다. */
#define STEP_LOG_INTERVAL         30

/* 진단 모드 — 1 로 켜면 걸음 후보마다 간격(샘플)과 판정 결과를 찍는다.
 *
 * 계수가 배로 나올 때 원인을 가르는 유일한 방법이다. 간격 수열을 보면
 * 두 경우가 갈리고, 대응책이 서로 다르다:
 *
 *   균일 (75, 74, 76, 75 …)   신호 자체가 걸음의 2배 주파수로 진동한다.
 *   교대 (30, 45, 31, 44 …)   한 걸음이 두 봉우리로 갈라진 것이다.
 *
 * 실제로 이 로그가 '느린 보행에서만 두 배' 라는 결론을 냈다 (아래 검증 이력).
 * 임계값을 다시 만져야 할 일이 생기면 이것부터 켤 것. */
#define STEP_DEBUG_INTERVALS      0

/* -----------------------------------------------------------------
 * [모듈 내부 정적(static) 변수]
 *
 * 걸음은 한 블록(0.5초) 안에 완결되지 않는다. 리듬 검증은 4걸음(최대 8초)에
 * 걸쳐 이어지므로 상태를 지역 변수로 두면 블록 경계마다 리듬이 초기화된다.
 * 시각 단위는 전부 '샘플 수' 다 (100Hz → 100 = 1초).
 * ----------------------------------------------------------------- */
/* 중력 벡터 추정 (축별 저역통과).
 * 크기뿐 아니라 '방향' 이 필요해서 3축을 따로 유지한다 — 이 방향이 곧
 * 수직축이고, 거기에 투영해야 팔 스윙(수평)이 걸러진다.
 * z=1.0 으로 시작하는 것은 화면이 위를 보는 흔한 초기 자세를 가정한 것일 뿐,
 * 실제 자세로 수렴하는 데 0.5초면 충분하다. */
static float    s_gx = 0.0f;
static float    s_gy = 0.0f;
static float    s_gz = 1.0f;

static float    s_smooth  = 0.0f;       // 평활화된 수직 동적 가속도
static uint8_t  s_armed   = 1;          // 피크 검출 재무장 여부

static uint32_t s_sample_clock = 0;     // 부팅 후 처리한 총 샘플 수 (차이 연산만 하므로 랩어라운드 안전)
static uint32_t s_last_peak = 0;        // 직전 걸음 후보 시각
static uint32_t s_last_interval = 0;    // 직전 두 후보 사이 간격 (케이던스 산출에도 쓴다)
static uint32_t s_run = 0;              // 규칙적으로 이어진 후보 수

static uint32_t s_steps = 0;            // 누적 걸음 수
static uint32_t s_next_log = STEP_LOG_INTERVAL;
static uint16_t s_block_steps = 0;      // 이번 블록에서 인정된 걸음 수
static uint32_t s_walk_steps = 0;       // '이번 보행' 에서 인정된 걸음 수 (보행 종료 로그용)

/**
  * @brief  걸음 후보 하나를 리듬 게이트에 넣는다.
  *
  * 인정되면 s_steps / s_block_steps 를 직접 올린다.
  */
static void step_candidate(void)
{
    uint32_t now = s_sample_clock;
    uint32_t interval = (s_run == 0) ? 0U : (now - s_last_peak);

    /* 판정 결과를 문자열로 남겨 함수 끝에서 한 번만 찍는다.
     * 탈출 지점마다 printf 를 흩뿌리면 판정 흐름이 로그에 묻힌다. */
    const char *verdict;

    /* 리듬의 첫 후보 — 간격을 잴 상대가 없다 */
    if (s_run == 0)
    {
        s_run = 1;
        s_last_peak = now;
        s_last_interval = 0;
        verdict = "first";
    }
    /* 한 걸음이 만든 이중 피크. 기준점을 갱신하지 않는다 —
     * 여기서 갱신하면 진짜 다음 걸음까지의 간격이 짧게 측정되어
     * 멀쩡한 리듬이 '불규칙' 으로 판정된다. */
    else if (interval < STEP_MIN_INTERVAL)
    {
        verdict = "too-fast";
    }
    /* 너무 오래 비었다 → 이 후보를 새 리듬의 첫걸음으로 삼는다 */
    else if (interval > STEP_MAX_INTERVAL)
    {
        s_run = 1;
        s_last_peak = now;
        s_last_interval = 0;
        verdict = "restart";
    }
    /* 리듬 유사성 검사. 곱셈으로 비교해 나눗셈과 부동소수점을 모두 피한다. */
    else if ((s_last_interval != 0) &&
             (((interval > s_last_interval) ? (interval - s_last_interval)
                                            : (s_last_interval - interval)) * 100U)
             > (s_last_interval * STEP_RHYTHM_TOL_PCT))
    {
        s_run = 1;
        s_last_peak = now;
        s_last_interval = 0;
        verdict = "irregular";
    }
    else
    {
        s_run++;
        s_last_peak = now;
        s_last_interval = interval;

        if (s_run == STEP_RUN_REQUIRED)
        {
            /* 리듬 확정 — 판정을 기다리느라 보류했던 앞선 걸음까지 한꺼번에 인정한다.
             * 이게 없으면 걷기 시작할 때마다 3걸음씩 증발한다. */
            s_steps += STEP_RUN_REQUIRED;
            s_block_steps = (uint16_t)(s_block_steps + STEP_RUN_REQUIRED);

            /* '=' 가 아니라 '+=' 다. 걷는 도중 리듬이 깨졌다 다시 잡히는 일은
             * 흔한데(방향 전환, 속도 변화), 여기서 덮어쓰면 그때마다 '이번 보행'
             * 집계가 4 로 리셋되어 누적값과 어긋난다. 이 값이 0 으로 돌아가는
             * 시점은 보행 종료(아래 타임아웃) 한 곳뿐이어야 한다. */
            s_walk_steps += STEP_RUN_REQUIRED;
            verdict = "LOCK +4";
        }
        else if (s_run > STEP_RUN_REQUIRED)
        {
            s_steps++;
            s_block_steps++;
            s_walk_steps++;
            verdict = "step +1";
        }
        else
        {
            /* 아직 보류 상태다 — 세지 않는다 */
            verdict = "pending";
        }
    }

#if STEP_DEBUG_INTERVALS
    printf("[STEP-D] 간격=%3lu run=%lu %s\r\n",
           (unsigned long)interval, (unsigned long)s_run, verdict);
#else
    (void)verdict;
#endif
}

void StepCounter_Init(void)
{
    StepCounter_Reset();
    printf("[ STEP ] module initialized. (ODR %dHz, %d걸음 리듬 확인)\r\n",
           BMI270_ACC_ODR_HZ, STEP_RUN_REQUIRED);
}

uint16_t StepCounter_ProcessBlock(const BMI270_Data_t *accel, uint16_t count)
{
    if (accel == NULL || count == 0) return 0;

    s_block_steps = 0;

    for (uint16_t i = 0; i < count; i++)
    {
        s_sample_clock++;

        /* [1단계] 중력 벡터 추정 — 축별로 느리게 따라가면 남는 것이 중력 방향이다.
         * 보행 성분(1~3Hz)은 이 필터를 통과하지 못하므로 자세만 걸러진다. */
        s_gx += STEP_GRAVITY_ALPHA * (accel[i].x - s_gx);
        s_gy += STEP_GRAVITY_ALPHA * (accel[i].y - s_gy);
        s_gz += STEP_GRAVITY_ALPHA * (accel[i].z - s_gz);

        float gnorm = sqrtf((s_gx * s_gx) + (s_gy * s_gy) + (s_gz * s_gz));

        /* 자유낙하 중이면 중력 방향 자체가 정의되지 않는다. 0 으로 나누는 것을
         * 막는 방어이기도 하고, 어차피 낙하 중에 걸음이 있을 리 없다. */
        if (gnorm < 0.1f) continue;

        /* [2단계] 순간 가속도를 중력축에 투영하고 중력 크기를 뺀다.
         *
         * 투영값에는 '중력 자신(=gnorm)' 과 '수직 동적 가속도' 만 남는다.
         * 수평 성분은 내적에서 0 으로 떨어진다 — 팔 앞뒤 스윙이 여기서 사라진다.
         * gnorm 을 빼는 것은 고정 1.0g 를 빼는 것과 다르다 (파일 상단 참조). */
        float proj = ((accel[i].x * s_gx) +
                      (accel[i].y * s_gy) +
                      (accel[i].z * s_gz)) / gnorm;

        float ac = proj - gnorm;

        /* [3단계] 4Hz 저역통과 — 손떨림과 충격 성분을 지운다 */
        s_smooth += STEP_SMOOTH_ALPHA * (ac - s_smooth);

        /* [4단계] 히스테리시스 피크 검출 */
        if (!s_armed)
        {
            if (s_smooth < STEP_PEAK_RESET_G) s_armed = 1;
        }
        else if (s_smooth > STEP_PEAK_THRESHOLD_G)
        {
            s_armed = 0;
            step_candidate();
        }
    }

    /* 보행 종료 검출.
     *
     * 이정표 로그는 문턱을 넘을 때만 찍히므로, 걷다가 멈추면 마지막 30걸음
     * 남짓이 로그에 나타나지 않는다. 실측에서 정작 필요한 건 그 최종값이다.
     * 마지막 걸음 후 시간 게이트를 넘기면 이번 보행의 총계를 한 번 찍는다.
     *
     * 여기서 리듬을 끊어도 동작은 달라지지 않는다. 지금까지는 다음 후보가
     * step_candidate() 안에서 '간격 초과' 로 걸러지며 같은 초기화를 했다. */
    if (s_run > 0 && (s_sample_clock - s_last_peak) > STEP_MAX_INTERVAL)
    {
        if (s_run >= STEP_RUN_REQUIRED)
        {
            printf("[ STEP ] 보행 종료 — 이번 %lu 걸음 / 누적 %lu 걸음\r\n",
                   (unsigned long)s_walk_steps, (unsigned long)s_steps);
        }
        s_run = 0;
        s_last_interval = 0;
        s_walk_steps = 0;
    }

    if (s_steps >= s_next_log)
    {
        printf("[ STEP ] %lu 걸음 (케이던스 %u spm)\r\n",
               (unsigned long)s_steps, (unsigned)StepCounter_GetCadence());
        s_next_log = s_steps + STEP_LOG_INTERVAL;
    }

    return s_block_steps;
}

uint32_t StepCounter_GetSteps(void)
{
    return s_steps;
}

uint16_t StepCounter_GetCadence(void)
{
    /* 리듬이 확정되기 전 값은 걸음이라고 볼 근거가 없다 */
    if (s_run < STEP_RUN_REQUIRED || s_last_interval == 0) return 0;

    /* 마지막 걸음 이후 시간 게이트를 넘겼으면 이미 멈춰 선 것이다 */
    if ((s_sample_clock - s_last_peak) > STEP_MAX_INTERVAL) return 0;

    return (uint16_t)((60U * (uint32_t)BMI270_ACC_ODR_HZ) / s_last_interval);
}

void StepCounter_SetSteps(uint32_t steps)
{
    s_steps = steps;
    s_next_log = steps + STEP_LOG_INTERVAL;
}

void StepCounter_Reset(void)
{
    /* 크기 1g 짜리 벡터로 시작한다. 0 벡터에서 시작하면 필터가 수렴하는
     * 0.5초 동안 ac 가 1g 부근에 머물러 임계값을 계속 넘고, 부팅할 때마다
     * 가짜 걸음이 무더기로 쌓인다. */
    s_gx = 0.0f;
    s_gy = 0.0f;
    s_gz = 1.0f;

    s_smooth  = 0.0f;
    s_armed   = 1;

    s_sample_clock  = 0;
    s_last_peak     = 0;
    s_last_interval = 0;
    s_run           = 0;

    s_steps       = 0;
    s_next_log    = STEP_LOG_INTERVAL;
    s_block_steps = 0;
    s_walk_steps  = 0;
}
