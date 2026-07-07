# protocol — 공통 통신 프로토콜

전 모듈(STM32 · 중계/알림 노드 · 서버 · 클라이언트)이 공유하는 패킷 정의입니다.

> ⚠️ **이 폴더의 변경은 전 모듈 재빌드를 유발합니다.**
> 변경 시 반드시 팀 전체 합의 → PR → 각 모듈 담당자 확인 순서로 진행하세요.

## 통신 구간별 프로토콜

| 구간 | 방식 | 정의 위치 |
| --- | --- | --- |
| STM32 → 중계 노드 | UART 바이너리 프레임 (header + payload + CRC16) | `protocol.h` |
| 중계 노드 → 중앙 서버 | Wi-Fi/TCP, 동일 프레임 캡슐화 | `protocol.h` |
| 서버 ↔ 알림 노드 | MQTT (토픽·명령 코드) | `protocol.h` (토픽 매크로) |
| 서버 → Qt 클라이언트 (영상) | TCP 바이너리 프레임 (헤더 + JPEG) — v1 평문, TLS 적용 예정 | `video_stream.h` |
| 서버 ↔ Qt 클라이언트 (제어/이벤트) | TCP + OpenSSL(TLS), JSON 메시지 | `docs/`에 명세 예정 |

## 서버 내부 모듈 간 데이터 계약 (video → core)

| 필드 | 설명 |
| --- | --- |
| `channel_id` | CCTV 채널 (입소자·웨어러블 매핑 키) |
| `occupancy` | 구역 내 인원 수 (WiseAI 메타데이터) |
| `bbox` | 바운딩 박스 좌표 및 변화량 |
| `slip_fall` | 카메라 자체 Slip&Fall 플래그 |
| `timestamp` | 이벤트 시각 |
