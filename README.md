# 🏥 다보이조 — 요양원 어르신 안전 모니터링 시스템

**웨어러블 · CCTV 융합 기반 낙상/이탈 감지 및 상태 모니터링**

> 입소자 웨어러블(STM32)과 한화비전 지능형 CCTV를 다중 교차 검증으로 연동하여,
> 낙상 사고의 골든타임을 확보하는 지능형 실버케어 관제 솔루션입니다.

- **팀**: 다보이조 (VEDA 4기 최종 프로젝트)
- **기간**: 2026. 7. 2 ~ 9. 1 (9주)
- **팀원**: 전승현(팀장) · 김예훈 · 박민용 · 이교민 · 홍성준

---

## 1. 시스템 구성

| 계층 | 구성 요소 | 역할 |
| --- | --- | --- |
| **수집** | 한화비전 CCTV (PNO-A9081RG / PNM-C16083RVQ) | RTSP 스트리밍, WiseAI 객체 인식·낙상 감지 메타데이터 |
| **수집** | 웨어러블 (STM32) | IMU 낙상 감지, PPG 심박, 피부온도 수집 → UART 송신 |
| **수집** | 중계 노드 (RPi 4) | UART 수신 → 버퍼링(단절 대비) → Wi-Fi/TCP로 서버 중계 |
| **처리** | 중앙 서버 (RPi 4) | RTSP 4채널 수신·디코딩, OpenCV 영상처리(저조도·ROI), 다중 교차 검증 최종 낙상 판정, 케어 로그 DB, OpenSSL |
| **출력** | 알림 노드 (RPi 4) | MQTT 구독 → 오디오/LED 시청각 알림 |
| **출력** | Qt 관제 클라이언트 | 4분할 모니터링, 낙상 시 강제 팝업·확대, 로그 조회, 알림 원격 해제 |

**낙상 판정 로직**: [웨어러블 충격 패킷] AND [비전 형태 변화] 동시 성립 시 확정.
비전 판단 불가 5초 지속 시 페일세이프로 차상위 경보 발생.

## 2. 폴더 구조

```
daboyijo/
 ├─ server/          # 중앙 서버 (RPi 4)
 │   ├─ video/       #   RTSP 4채널 수신·디코딩, ONVIF, OpenCV 영상처리
 │   └─ core/        #   교차 검증 룰엔진, DB/로그, OpenSSL 통신
 ├─ client/          # Qt 관제 클라이언트 (Windows/Linux)
 ├─ firmware/        # STM32 웨어러블 펌웨어
 ├─ relay-node/      # 중계 노드 (RPi 4) — UART 수신·버퍼링·서버 송신
 ├─ alert-node/      # 알림 노드 (RPi 4) — MQTT 구독, 오디오/LED 드라이버
 ├─ protocol/        # ★ 전 모듈 공통 패킷 구조체·메시지 타입 정의
 └─ docs/            # 기획서, 설계서, 프로토콜 명세
```

## 3. 모듈별 담당

| 모듈 | 담당 | 비고 |
| --- | --- | --- |
| `server/video` | 박민용, 홍성준 | 필수 요구사항 최다 — 최우선 착수 |
| `server/core` | 박민용, 홍성준 | 통신 프로토콜 정의 주도 |
| `firmware` | 김예훈, 이교민, 전승현 | 무선 통신 조기 검증 필수 |
| `relay-node` / `alert-node` | 김예훈, 이교민, 전승현 | 디바이스 드라이버 요구사항 |
| `client` | 전체 | UI/UX 및 강제 팝업 로직 |

## 4. 기술 스택

- **언어/빌드**: C/C++, Make
- **서버**: Raspberry Pi OS, OpenCV, OpenSSL, ONVIF, RTSP, MQTT
- **클라이언트**: Qt (Windows/Linux 크로스플랫폼)
- **펌웨어**: STM32CubeIDE, HAL, UART/I2C
- **협업**: Git/GitHub, Jira(KAN), Notion

## 5. 브랜치 전략

```
main     ← 발표/시연 가능한 안정 버전 (마일스톤 단위 머지)
develop  ← 통합 브랜치. 모든 PR의 대상 (기본 브랜치)
feat/... ← 기능 브랜치. 예: feat/server-rtsp-4ch, feat/firmware-imu-fall
fix/...  ← 버그 수정
```

**규칙**

1. `main`, `develop`에 직접 push 금지 — 반드시 PR
2. PR 리뷰어 1명 이상 (같은 모듈 담당자 우선)
3. 브랜치명은 `타입/모듈-작업` 형식

## 6. 커밋 컨벤션

```
feat: RTSP 4채널 수신 파이프라인 구현 (KAN-12)
```

- 타입: `feat` `fix` `refactor` `docs` `chore` `test`
- 끝에 Jira 이슈 키 `(KAN-nn)` 를 붙이면 Jira에 자동 연동됩니다.
