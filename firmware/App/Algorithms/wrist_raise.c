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
  * ── 왜 히스테리시스와 정지 조건이 필요한가 ──────────────────────────
  *
  *   문턱 하나로 판정하면 경계 각도에서 화면이 깜빡인다. 켜지는 각도(55°)를
  *   꺼지는 각도(70°)보다 좁게 잡아 그 진동을 없앤다.
  *
  *   걷을 때 팔이 앞뒤로 흔들리면 매 스윙마다 화면이 잠깐씩 하늘을 본다.
  *   그때마다 켜지면 배터리가 남아나지 않는다. 그래서 켜는 조건에만
  *   '거의 정지' 를 추가로 요구한다 — 시계를 보는 사람은 손목을 멈춘다.
  *   끄는 쪽에는 걸지 않는다. 팔을 휘두르며 내릴 때도 꺼져야 하기 때문이다.
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

#define WR_ON_COS            0.57f   /* 하늘 기준 55° 이내면 '본다' */
#define WR_OFF_COS           0.34f   /* 70° 밖으로 나가면 '내렸다' */

/* 정지 판정: 총 가속도 크기가 1g 에서 이만큼 안쪽이면 흔들리지 않는 것으로 본다.
 * 시계를 보려고 멈춘 손목은 0.05g 안쪽, 보행 중 스윙은 0.3~1g 를 넘나든다. */
#define WR_STILL_G           0.25f

#define WR_ON_HOLD_SAMPLES   15      /* 150ms 유지되어야 켠다 */
#define WR_OFF_HOLD_SAMPLES  40      /* 400ms 유지되어야 끈다 */

/* ── 상태 ──────────────────────────────────────────────────────────── */
static float    s_gx, s_gy, s_gz;      /* 저역통과된 중력 벡터 (g) */
static float    s_facing;              /* 화면 법선의 상향 성분 (-1 ~ +1) */
static uint8_t  s_seeded;              /* 중력 벡터에 첫 샘플을 심었는가 */
static uint8_t  s_raised;              /* 디바운스까지 끝난 최종 판정 */
static uint8_t  s_raise_event;         /* 상승 엣지 (소비형) */
static uint16_t s_on_run, s_off_run;   /* 조건이 연속으로 성립한 샘플 수 */

void WristRaise_Init(void)
{
    s_gx = s_gy = s_gz = 0.0f;
    s_facing = 0.0f;
    s_seeded = 0;
    s_raised = 0;
    s_raise_event = 0;
    s_on_run = s_off_run = 0;
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
        float amag = sqrtf(ax * ax + ay * ay + az * az);
        uint8_t still = (fabsf(amag - 1.0f) < WR_STILL_G);

        if (!s_raised)
        {
            if (facing >= WR_ON_COS && still)
            {
                if (++s_on_run >= WR_ON_HOLD_SAMPLES)
                {
                    s_raised = 1;
                    s_raise_event = 1;
                    s_on_run = 0;
                    s_off_run = 0;
                }
            }
            else
            {
                s_on_run = 0;
            }
        }
        else
        {
            if (facing <= WR_OFF_COS)
            {
                if (++s_off_run >= WR_OFF_HOLD_SAMPLES)
                {
                    s_raised = 0;
                    s_off_run = 0;
                    s_on_run = 0;
                }
            }
            else
            {
                s_off_run = 0;
            }
        }
    }

#if WRIST_RAISE_DEBUG
    /* 블록이 0.5초마다 오므로 이 자체가 2Hz 로그다 */
    printf("[ WRIST ] g=(%+.2f %+.2f %+.2f) facing=%+.2f raised=%u\r\n",
           s_gx, s_gy, s_gz, s_facing, s_raised);
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
