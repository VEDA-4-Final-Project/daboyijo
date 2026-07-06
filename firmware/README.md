# firmware — STM32 웨어러블 펌웨어

담당: 김예훈, 이교민, 전승현

- IMU 낙상 감지(충격 임계·자유낙하 인터럽트), PPG 심박, 피부온도 수집 (I2C)
- 착용/미착용 판정 (생체 신호 단절 기반)
- UART로 중계 노드에 프레임 송신 — 패킷 정의는 `../protocol/protocol.h`
- 저전력 운영

개발 환경: STM32CubeIDE, ARM GCCㅊㅊㅊㅊㅊㅊ
