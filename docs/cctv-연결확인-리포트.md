# CCTV 연결 확인 리포트 (2주차 산출물)

- **일자**: 2026-07-07
- **작성**: 박민용 (중앙 서버 파트)
- **목표**: 한화비전 카메라 연결 및 RTSP 4채널 스트림 수신 확인 ✅ 달성

## 1. 카메라 정보

| 항목 | 값 |
| --- | --- |
| 모델 | 한화비전 PNM-C16083RVQ (AI 4MP 4채널 IR 멀티디렉셔널) |
| 펌웨어 | 25.01.09_20260106_R1406 |
| IP 주소 | 172.20.35.85 (Wisenet Device Manager로 할당) |
| ONVIF | v22.12, 엔드포인트 `http://172.20.35.85/onvif/device_service` |
| 계정 | admin (비밀번호는 팀 내부 공유 — 저장소에 기록하지 않음) |

## 2. RTSP 스트림 확인 결과

**URL 형식** (ffprobe 및 ONVIF Device Manager 양쪽으로 검증):

```
rtsp://<계정>:<비밀번호>@172.20.35.85:554/<채널 0~3>/profile2/media.smp
```

- 멀티디렉셔널 카메라라 한 대에서 채널 4개(센서 4개)가 나옴
- 채널별 스냅샷 캡처로 4채널이 각각 다른 센서임을 확인

**프로파일 설정**: 기본 profile2가 4MP(2592×1520) @30fps였으나, RPi 4의 4채널
동시 디코딩 부하를 고려해 **H.264 / 1280×720 / 15fps**로 조정
(기획서의 "10~15fps 고정" 전략 반영)

## 3. 서버 수신 검증 (Raspberry Pi 4)

`server/` 골격 코드로 4채널 동시 수신 테스트:

```
[ch0] OK 15fps  [ch1] OK 15fps  [ch2] OK 15fps  [ch3] OK 15.2fps
```

- 4채널 전부 안정적으로 15fps 수신 확인
- 스트림 끊김 시 3초 백오프 자동 재연결 동작 확인

### 구현된 골격 구조

```
server/
 ├─ Makefile                # g++ / pkg-config opencv4
 ├─ src/main.cpp            # 설정 로드 → 채널별 수신 → 5초마다 FPS 리포트
 ├─ video/
 │   ├─ rtsp_client.*       # 채널별 수신·디코딩 스레드 (자동 재연결)
 │   └─ frame_queue.hpp     # 수신→처리 스레드 간 큐 (가득 차면 오래된 프레임 드롭)
 └─ config/
     └─ cameras.conf.example  # RTSP URL 설정 예시 (실제 계정 포함 conf는 gitignore)
```

## 4. 트러블슈팅 기록

| 증상 | 원인 | 해결 |
| --- | --- | --- |
| ch1만 `505 RTSP Version not supported` | cameras.conf의 해당 줄에 공백/개행 문자 혼입 → RTSP 요청 라인 깨짐 | 해당 줄 재작성. 재발 방지로 설정 파서에 공백·CR 자동 제거 및 경고 추가 |
| Wisenet Device Manager 검색 안 됨 | (예방 조치) VirtualBox Host-Only 어댑터로 검색 패킷 유출 가능 | 검색 시 불필요한 가상 어댑터 비활성화 |

## 5. 다음 단계 (3주차)

- 4채널 수신·디코딩 파이프라인 본격 구현 (프레임 소비부에 처리 스레드 연결)
- OpenCV 전처리(저조도 보정) 및 채널별 최신 프레임 버퍼 설계
- ONVIF 연동: `GetStreamUri`로 RTSP URL 자동 조회 (하드코딩 제거)
