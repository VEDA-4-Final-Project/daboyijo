# relay-node — 중계 노드 (Raspberry Pi 4)

담당: 김예훈, 이교민, 전승현

- STM32로부터 UART 프레임 수신·CRC 검증
- 네트워크 단절 대비 내부 큐 버퍼링 (데이터 유실 방지)
- Wi-Fi/TCP로 중앙 서버에 전달, 타임스탬프 보정
