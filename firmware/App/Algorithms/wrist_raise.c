/**
  ******************************************************************************
  * @file    wrist_raise.c
  * @brief   손목 들기 감지 — 화면을 켤 순간을 고른다
  ******************************************************************************
  *
  * ── 무엇을 보는가 ───────────────────────────────────────────────────
  *
  *   가속도계는 정지 상태에서 '위를 향한 축' 에 +1g 를 읽는다. 그래서 중력만
  *   남기면(저역통과) 그 벡터가 곧 기기의 자세다. 화면 법선이 하늘을 향할수록
  *   법선축 성분이 +1 에 가까워진다 — 이 한 값(facing)으로 판정한다.
  *
  *   자이로는 쓰지 않는다. 전원이 꺼져 있고(bmi270.c PWR_CTRL 참조) 각도만
  *   필요한 판정에 자이로를 켜면 소비 전류만 늘어난다.
  *
  * ── 자세와 점등을 분리한 이유 ───────────────────────────────────────
  *
  *   이 파일은 두 가지를 따로 낸다.
  *
  *     자세(IsRaised)      각도만 본다. 히스테리시스로 경계 떨림을 없앤다
  *                         (켜짐 60° / 꺼짐 70°). 화면을 '끌 때' 쓴다.
  *     점등(RaiseEvent)    '움직인 뒤 화면이 위를 본 채 멈춘 순간'. 켤 때 쓴다.
  *
  *   처음에는 점등을 자세의 상승 엣지 하나로 만들었는데, 타임아웃으로 꺼진 뒤
  *   팔을 계속 들고 있으면(책상에 팔을 올려둔 자세) 자세가 계속 '들림' 이라
  *   새 엣지가 영영 오지 않아 다시 켤 방법이 없었다.
  *
  *   그래서 '무장(armed)' 을 둔다. 손목을 튕기거나 팔을 내리면 무장되고,
  *   무장된 상태에서 멈추면 켜진다. 움직임 없이 자세만으로 켜지는 일은
  *   여전히 없으므로, 팔을 든 채 가만히 있다고 계속 켜지지는 않는다.
  *
  *   정지 조건을 켜는 쪽에만 거는 것은 그대로다. 걸을 때 팔 스윙마다 화면이
  *   잠깐씩 하늘을 보는데 그때마다 켜지면 배터리가 남아나지 않는다.
  *   끄는 쪽에 걸지 않는 이유는 팔을 휘두르며 내릴 때도 꺼져야 하기 때문이다.
  *
  * ── 축 설정 ─────────────────────────────────────────────────────────
  *
  *   ⚠ WRIST_RAISE_AXIS 는 조립 방향에 따라 반드시 실측으로 맞춰야 한다.
  *     아래 WRIST_RAISE_DEBUG 를 1 로 두고 화면을 하늘로 향하게 놓았을 때
  *     facing 이 +1 에 가까워지는 축/부호를 고르면 끝이다.
  *
  ******************************************************************************
  */
#include "wrist_raise.h"
#include <math.h>
#include <stdio.h>

/* ── 조립에 맞춰 고칠 것 ───────────────────────────────────────────── */
#define WR_AXIS_X            0
#define WR_AXIS_Y            1
#define WR_AXIS_Z            2

/* 화면 법선(화면이 바라보는 방향)이 IMU 의 어느 축인가 */
#define WRIST_RAISE_AXIS     WR_AXIS_Z
/* 그 축의 + 방향이 화면 뒤쪽이면 1 로 뒤집는다 */
#define WRIST_RAISE_INVERT   0

/* 1 로 두면 1초마다 중력 벡터와 facing 을 USB CDC 로 찍는다 (축 맞출 때만) */
#define WRIST_RAISE_DEBUG    0

/* ── 판정 상수 ─────────────────────────────────────────────────────── */
#define WR_ODR_HZ            BMI270_ACC_ODR_HZ

/* 중력 저역통과 시정수 0.5초. 팔 스윙(1~2Hz)은 걸러내고 자세 변화는 따라간다. */
#define WR_GRAVITY_ALPHA     (1.0f / (0.5f * (float)WR_ODR_HZ))

#define WR_ON_COS            0.50f   /* 하늘 기준 60° 이내면 '본다' */
#define WR_OFF_COS           0.34f   /* 70° 밖으로 나가면 '내렸다' */

/* 정지 / 움직임 판정: 총 가속도 크기가 1g 에서 얼마나 벗어났는가.
 * 시계를 보려고 멈춘 손목은 0.05g 안쪽, 보행 중 스윙은 0.4g 를 쉽게 넘는다.
 * 두 문턱 사이에 틈을 둬서 경계에서 두 판정이 동시에 참이 되지 않게 한다. */
#define WR_STILL_G           0.30f
#define WR_MOTION_G          0.40f

#define WR_ON_HOLD_SAMPLES   15      /* 자세를 '들림' 으로 인정하는 유지 시간 150ms */
#define WR_OFF_HOLD_SAMPLES  40      /* 자세를 '내림' 으로 인정하는 유지 시간 400ms */
#define WR_SETTLE_SAMPLES    15      /* 움직임이 멎고 150ms 유지되면 점등 */

/* ── 상태 ──────────────────────────────────────────────────────────── */
static float    s_gx, s_gy, s_gz;      /* 저역통과된 중력 벡터 (g) */
static float    s_facing;              /* 화면 법선의 상향 성분 (-1 ~ +1) */
static uint8_t  s_seeded;              /* 중력 벡터에 첫 샘플을 심었는가 */
static uint8_t  s_raised;              /* 자세: 손목이 들려 있는가 (히스테리시스) */
static uint8_t  s_raise_event;         /* 점등 신호 (소비형) */
static uint8_t  s_armed;               /* 다음 점등을 받아들일 준비가 됐는가 */
static uint16_t s_on_run, s_off_run;   /* 자세 판정이 연속 성립한 샘플 수 */
static uint16_t s_settle_run;          /* 들린 채 멈춰 있은 샘플 수 */

void WristRaise_Init(void)
{
    s_gx = s_gy = s_gz = 0.0f;
    s_facing = 0.0f;
    s_seeded = 0;
    s_raised = 0;
    s_raise_event = 0;
    s_armed = 0;
    s_on_run = s_off_run = s_settle_run = 0;
}

void WristRaise_ProcessBlock(const BMI270_Data_t *accel, uint16_t count)
{
    if (accel == NULL || count == 0) return;

    for (uint16_t i = 0; i < count; i++)
    {
        const float ax = accel[i].x, ay = accel[i].y, az = accel[i].z;

        /* 첫 샘플은 그대로 심는다.
         * 0 에서 시작하면 시정수만큼(0.5초) 중력이 실제보다 작게 잡혀
         * 부팅 직후 한 블록이 통째로 오판된다. */
        if (!s_seeded)
        {
            s_gx = ax; s_gy = ay; s_gz = az;
            s_seeded = 1;
        }
        else
        {
            s_gx += WR_GRAVITY_ALPHA * (ax - s_gx);
            s_gy += WR_GRAVITY_ALPHA * (ay - s_gy);
            s_gz += WR_GRAVITY_ALPHA * (az - s_gz);
        }

        /* 중력 벡터를 단위화해 축 성분을 각도의 코사인으로 만든다.
         * 크기로 나누지 않으면 가속 구간에서 벡터가 길어져 문턱이 흔들린다. */
        float norm = sqrtf(s_gx * s_gx + s_gy * s_gy + s_gz * s_gz);
        if (norm < 0.1f) continue;      /* 자유낙하 등 — 자세를 말할 수 없다 */

#if   WRIST_RAISE_AXIS == WR_AXIS_X
        float facing = s_gx / norm;
#elif WRIST_RAISE_AXIS == WR_AXIS_Y
        float facing = s_gy / norm;
#else
        float facing = s_gz / norm;
#endif
#if WRIST_RAISE_INVERT
        facing = -facing;
#endif
        s_facing = facing;

        /* 순간 가속도 크기 — 1g 에서 얼마나 벗어났는가가 곧 흔들림의 양이다 */
        float dev = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
        uint8_t still  = (dev < WR_STILL_G);
        uint8_t moving = (dev > WR_MOTION_G);

        /* ── 1) 재무장 ──────────────────────────────────────────────────
         * 점등을 '자세가 내림→들림 으로 바뀌는 순간' 하나에만 걸면, 타임아웃으로
         * 꺼진 뒤 팔을 계속 들고 있는 동안에는 다시 켤 방법이 사라진다.
         * 책상에 팔을 올려둔 자세가 정확히 그 상태다.
         *
         * 그래서 팔을 내렸을 때뿐 아니라 손목을 한 번 튕겼을 때도 다시 받아들인다.
         * '움직임 없이 자세만으로' 켜지는 일은 여전히 없다 — 무장은 반드시
         * 움직임이나 내림을 거쳐야 한다. */
        if (moving || facing <= WR_OFF_COS) s_armed = 1;

        /* ── 2) 자세 상태 ───────────────────────────────────────────────
         * 소등 판단에만 쓴다. 각도만 보고 정지 여부는 따지지 않는다 —
         * 팔을 휘두르며 내릴 때도 '내렸다' 로 판정되어야 하기 때문이다. */
        if (!s_raised)
        {
            if (facing >= WR_ON_COS)
            {
                if (++s_on_run >= WR_ON_HOLD_SAMPLES) { s_raised = 1; s_on_run = 0; s_off_run = 0; }
            }
            else s_on_run = 0;
        }
        else
        {
            if (facing <= WR_OFF_COS)
            {
                if (++s_off_run >= WR_OFF_HOLD_SAMPLES) { s_raised = 0; s_off_run = 0; s_on_run = 0; }
            }
            else s_off_run = 0;
        }

        /* ── 3) 점등 신호 ───────────────────────────────────────────────
         * '움직인 뒤, 화면이 위를 본 채로 멈춘 순간' 이 사람이 시계를 보는 동작이다.
         * 걸으면서 드는 경우도 여기서 걸린다 — 보행 자체가 무장을 시켜두므로
         * 손목을 잠깐 고정하는 순간 바로 켜진다. */
        if (s_armed && facing >= WR_ON_COS && still)
        {
            if (++s_settle_run >= WR_SETTLE_SAMPLES)
            {
                s_raise_event = 1;
                s_armed = 0;
                s_settle_run = 0;
            }
        }
        else
        {
            s_settle_run = 0;
        }
    }

#if WRIST_RAISE_DEBUG
    /* 블록이 0.5초마다 오므로 이 자체가 2Hz 로그다 */
    printf("[ WRIST ] g=(%+.2f %+.2f %+.2f) facing=%+.2f raised=%u armed=%u\r\n",
           s_gx, s_gy, s_gz, s_facing, s_raised, s_armed);
#endif
}

uint8_t WristRaise_IsRaised(void)
{
    return s_raised;
}

uint8_t WristRaise_ConsumeRaiseEvent(void)
{
    uint8_t e = s_raise_event;
    s_raise_event = 0;
    return e;
}

float WristRaise_GetFacing(void)
{
    return s_facing;
}
