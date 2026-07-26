#ifndef FALL_DETECTION_H_
#define FALL_DETECTION_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "bmi270.h"

/* ==================================================================== */
/* 1. 매크로 및 상수 정의 (상보필터 및 낙상 임계값)                     */
/* ==================================================================== */
#define DT                  0.01f   // 샘플링 주기 (10ms = 100Hz)
#define ALPHA               0.96f   // 상보필터 계수 (자이로 96%, 가속도 4%)

#define IMPACT_THRESHOLD    2.5f    // 충격 감지 임계값 (예: 2.5g)
#define GYRO_STILL_THRESH   15.0f   // 정지 상태 판단 자이로 총합 임계값 (예: 15 dps 이하)
#define ANGLE_CHANGE_THRESH 45.0f   // 자세 변화 감지 임계값 (충격 전/후 45도 이상 변화)

#define OBSERVATION_TIME_MS 1500    // 충격 후 관찰 시간 (1.5초)


/* ==================================================================== */
/* 2. 알고리즘 상태 머신 열거형 (Enum)                                  */
/* ==================================================================== */
typedef enum {
    STATE_NORMAL = 0,    // 평상시 (충격 감지 대기)
    STATE_IMPACTED,      // 충격 감지 됨 (1.5초간 자세 변화 및 정지 상태 관찰)
    STATE_FALLEN         // 낙상 확정
} FallState_t;


/* ==================================================================== */
/* 3. 알고리즘 내부 데이터 보관용 구조체                                */
/* ==================================================================== */
typedef struct {
    FallState_t current_state;      // 현재 알고리즘 상태
    uint32_t    impact_time;        // 충격이 발생한 시스템 틱 타임 기록

    // 현재 각도 저장용 (상보필터 결과)
    float roll;
    float pitch;

    // 충격 직전의 각도 백업용 (자세 변화 판별을 위함)
    float pre_impact_roll;
    float pre_impact_pitch;

} FallAlgo_Data_t;


/* ==================================================================== */
/* 4. 외부 공개 함수 (API)                                              */
/* ==================================================================== */
/**
 * @brief 낙상 감지 알고리즘 초기화 (내부 변수 0으로 세팅)
 */
void FallDetection_Init(void);

/**
 * @brief 10ms 주기마다 호출되어 낙상 여부를 판별하는 핵심 엔진
 * @param accel bmi270에서 읽어온 3축 가속도 데이터 포인터
 * @param gyro  bmi270에서 읽어온 3축 자이로 데이터 포인터
 */
void FallDetection_Update(BMI270_Data_t *accel, BMI270_Data_t *gyro);


#ifdef __cplusplus
}
#endif

#endif /* FALL_DETECTION_H_ */
