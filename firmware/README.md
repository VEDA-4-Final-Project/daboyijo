# firmware — STM32 웨어러블 펌웨어

담당: 김예훈, 이교민, 전승현

- 하드웨어 플랫폼: STM32F411CEU6 (BlackPill) 메인 보드 제어
- 모션 및 낙상 감지: BMI270(IMU) 기반 낙상 감지(충격 임계·자유낙하 인터럽트), 만보기 기능 및 SOS 노크 인식
- 생체 신호 수집: MAX30102(PPG 심박) 및 MAX30205(피부온도) 실시간 데이터 수집 (I2C 공유 버스)
- 착용/미착용 판정: 생체 신호 단절 및 데이터 유효성 기반 판정 알고리즘 적용
- 데이터 통신: HM-10(BLE, USART1)을 활용해 중계 노드로 프레임 송신 — 패킷 정의는 ../protocol/protocol.h
- 디버깅 및 업로드: ST-LINK 없는 환경으로, USB DFU 모드 업로드 및 USART2(printf) 시리얼 모니터링 로그 디버깅 수행
- 저전력 운영: 센서 수집 주기 최적화 및 MCU Stop/Sleep 모드 연동을 통한 배터리 세이빙

개발 환경: STM32CubeIDE v1.18.1, STM32CubeMX v6.14.1, ARM GCC