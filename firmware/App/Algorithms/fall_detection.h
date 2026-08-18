#ifndef APP_ALGORITHMS_FALL_DETECTION_H_
#define APP_ALGORITHMS_FALL_DETECTION_H_

#include "bmi270.h"

/* 낙상 상태 정의 */
typedef enum {
    FALL_NONE = 0,      // 정상 상태
    FALL_DETECTED       // 낙상 확정
} FallState_t;

/* 낙상 모듈 초기화 */
void FallDetection_Init(void);

/**
  * FIFO 블록 단위 낙상 판별
  *
  * @param accel   가속도 배열 (g 단위), BMI270 FIFO 한 블록
  * @param count   배열 유효 길이
  * @param is_worn 착용 여부 (PPG 기반). 미착용 시 낙상 확정을 보류한다.
  * @return        블록 처리 중 낙상이 확정되면 FALL_DETECTED
  */
FallState_t FallDetection_ProcessBlock(const BMI270_Data_t *accel, uint16_t count, uint8_t is_worn);

/**
  * 직전 블록의 움직임 강도 — 블록 내 SVM 의 '표준편차' (g 단위).
  *
  * |SVM-1g| 의 평균도, |SVM-1g| 의 표준편차도 아니다. 절댓값을 거치지 않은
  * SVM 자체의 표준편차다. 가속도계 스케일 오차로 정지 상태에서도 SVM 이
  * 1.04 로 읽히는데, 평균 편차를 쓰면 그 오차가 0.04 라는 가짜 바닥값으로
  * 깔려 '정지'와 '움직임'을 구분하지 못한다. 표준편차는 오차가 상수로
  * 실리면 0 이 되므로 이 문제가 없다.
  * (상세 근거와 알려진 정밀도 한계는 fall_detection.c 상단 '움직임 지표' 참조)
  *
  * PPG 모션 블랭킹이 자이로 대신 사용한다. 순간값이 아닌 블록 전체 통계라
  * 블랭킹 판단 근거로는 자이로 1회 샘플보다 안정적이다.
  *
  * 이 값을 실제로 판정에 쓰는 곳 (heart_rate_calc.c):
  *   MOTION_BLANKING_THRESHOLD_G  0.05   PPG 모션 블랭킹
  *   SPO2_MOTION_LIMIT_G          0.008  SpO2 정지 판정 (훨씬 엄격)
  *
  * @return 정지 시 0.001~0.003, 움직일 때 0.1 이상
  */
float FallDetection_GetBlockMotion(void);

/**
  * 내부 상태 전체 초기화 — 상태머신, SVM 이력 링버퍼, 정지 검증 카운터.
  * 이력 버퍼는 0 이 아니라 1.0g(정지 상태)로 채운다. 되짚기 깊이는 어차피
  * s_hist_filled 로 막혀 있어 0 이어도 동작하지만, 0 은 그 자체가 자유낙하로
  * 읽히는 값이라 이중 방어로 남겨둔다.
  */
void FallDetection_Reset(void);

#endif /* APP_ALGORITHMS_FALL_DETECTION_H_ */
