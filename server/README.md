# server — 중앙 서버 (Raspberry Pi 4)

담당: 박민용, 홍성준

| 하위 폴더 | 역할 |
| --- | --- |
| `video/` | RTSP 4채널 수신·디코딩, ONVIF 연동, OpenCV 영상처리(저조도 보정·침상 ROI 마스킹), WiseAI 메타데이터 파싱, 요양보호사 진입 감지 |
| `core/` | 다중 교차 검증 룰엔진(최종 낙상 판정), 입소자 DB·케어 로그, OpenSSL 서버측 보안 통신, MQTT 발행 |

## 빌드 및 실행 (Raspberry Pi OS)

```bash
# 의존성
sudo apt install libopencv-dev pkg-config

# 카메라 설정 (계정/IP 입력 — cameras.conf는 커밋되지 않음)
cp config/cameras.conf.example config/cameras.conf

make        # build/daboyijo-server 생성
make run    # config/cameras.conf 로 4채널 수신 시작, 5초마다 채널별 FPS 출력
```

RTSP를 TCP로 받으려면(패킷 유실 시 화면 깨짐 방지):

```bash
OPENCV_FFMPEG_CAPTURE_OPTIONS="rtsp_transport;tcp" make run
```

## 현재 구조

```
server/
 ├─ Makefile
 ├─ src/main.cpp        # 진입점 — 설정 로드, 채널별 수신 시작, FPS 리포트
 ├─ video/
 │   ├─ rtsp_client.*   # 채널 1개 수신·디코딩 워커 (자동 재연결)
 │   └─ frame_queue.hpp # 수신→처리 스레드 간 프레임 큐 (가득 차면 오래된 것부터 드롭)
 ├─ core/               # (예정) 교차 검증 룰엔진, DB/로그, OpenSSL, MQTT
 └─ config/
     └─ cameras.conf.example  # 채널별 RTSP URL 예시
```
