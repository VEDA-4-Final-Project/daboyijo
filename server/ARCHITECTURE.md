# 서버 모듈 구조 (충돌 없이 협업하기)

`main.cpp`에 모든 기능의 로직이 섞여 있어 병합 충돌이 잦았던 문제를 해결하기 위해,
서버를 **기능별 모듈**로 분리했다. 핵심 원칙:

> **기능 개발은 자기 모듈 파일에서만. `main.cpp`는 "조립도"라서 거의 안 바뀐다.**

## 디렉토리 맵

```
server/
├── src/
│   ├── main.cpp            ← 조립도: 모듈 생성 + 배선만 (~130줄)
│   ├── config.*            ← cameras.conf 파싱 (공용)
│   └── system_stats.*      ← CPU/온도 (공용)
├── modules/                ← ★ 기능별 모듈 (새로 생김)
│   ├── video_pipeline.*    ← [공용] 영상 메인 루프: 큐→스테이지→인코딩→송출
│   ├── ai_worker.*         ← [공용] AI 전담 스레드 — 채널당 1개 (프로세서 등록형)
│   ├── detection_store.*   ← [공용] 감지 이력 저장 + 프레임-좌표 시간 매칭
│   ├── stats_reporter.*    ← [공용] 5초 주기 상태 리포트
│   ├── fall_module.*       ← [낙상감지] FallDetector+MoveNet 배선, 튜닝값
│   ├── caregiver_module.*  ← [요양사감지] 옷색 판정+케어타이머+DB 기록
│   ├── blackbox_module.*   ← [블랙박스] 클립 저장+HTTP 서빙
│   └── telegram_module.*   ← [보호자 알림] 낙상/침상탈출 확정 시 텔레그램 전송 (데모용)
├── core/                   ← 저수준 부품: fall_detector, stream_server, database, clip_http_server
└── video/                  ← 저수준 부품: rtsp, privacy_masker, caregiver_detector, care_timer, pose_estimator, blackbox_recorder
```

## 누가 어떤 파일을 수정하는가

| 기능 | 수정하는 파일 |
|---|---|
| 낙상감지 | `modules/fall_module.*`, `core/fall_detector.*`, `video/pose_estimator.*` |
| 블러처리 | `video/privacy_masker.*` |
| 요양사 감지 | `modules/caregiver_module.*`, `video/caregiver_detector.*`, `video/care_timer.*` |
| 블랙박스 | `modules/blackbox_module.*`, `video/blackbox_recorder.*`, `core/clip_http_server.*` |
| 보호자 알림(텔레그램) | `modules/telegram_module.*` |
| RTSP/스트리밍 인프라 | `video/rtsp_av_client.*`, `core/stream_server.*`, `modules/video_pipeline.*` |

튜닝값(모델 경로, 추론 주기, 색 범위, 저장 시간 등)은 **각 모듈 .cpp 상단**에 모여 있다.

## 새 기능을 붙이는 방법

1. `modules/내기능_module.hpp/.cpp` 를 만든다 (Makefile은 wildcard라 자동 포함).
2. 필요한 연결 지점을 고른다:
   - **송출 영상을 가공**하고 싶다 (블러처럼) → `pipeline.addStage(...)`
   - **무거운 분석**을 하고 싶다 (AI 추론처럼) → `ai_worker.addProcessor(...)`
   - **메타데이터(감지 좌표)를 받고** 싶다 → `client->setDetectionCallback` 내부에 한 줄 추가
   - **낙상 등 이벤트에 반응**하고 싶다 → `fall.setFallCallback` 배선 블록에 한 줄 추가
3. `main.cpp`에는 모듈 생성 1줄 + 배선 몇 줄만 추가한다. **남의 배선 블록은 건드리지 않는다.**

## 협업 규칙 (병합 충돌 방지)

- 기능 브랜치에서 작업하고 PR은 항상 `develop`으로.
- PR 올리기 전에 `git pull origin develop` 후 로컬에서 develop을 머지해 충돌을 먼저 푼다.
- `main.cpp` 수정이 필요한 PR은 배선 블록 몇 줄 이내로 유지한다.
- 빌드 산출물(테스트 바이너리)·백업 파일(`.bak`)은 커밋하지 않는다.

## 스레드 구조 (참고)

- **RTSP 수신 스레드** (채널당 1개): 프레임→`FrameQueue`, 메타→`DetectionStore`/`FallModule`, 압축패킷→블랙박스
- **메인 스레드** (`VideoPipeline::run`): 15fps 제한 → 좌표 매칭 → 블러 → JPEG → 송출
- **AI 워커 스레드** (`AiWorker`, **채널당 1개**): 요양사 색 판정 → MoveNet 자세 판정 (채널별 최신 1장씩만 처리). 한 채널의 추론이 느려도 다른 채널 분석이 안 밀린다. 같은 채널은 항상 같은 스레드가 처리하므로 프로세서의 "채널별 상태"는 락 불필요 — 채널 간 공유 상태(FallDetector 등)만 각 모듈이 뮤텍스로 보호.
- **StreamServer 스레드들**: 클라이언트별 송신/수신(ROI·확인 신호)

스레드 간 공유 자원은 각 모듈이 내부 뮤텍스로 스스로 보호한다 — 모듈 밖에서 락을 잡을 필요 없음.
