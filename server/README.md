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
make run    # 4채널 수신 → 640x360 리사이즈 → JPEG 인코딩 파이프라인 실행
```

5초마다 채널별 수신/처리 fps, JPEG 크기, CPU 사용률, SoC 온도를 출력한다.
이 수치가 곧 **B안(서버 경유 영상 중계) 부하 벤치마크**다:

> 판정 기준 — CPU 70% 이하, 온도 70°C 이하, 전 채널 12fps 이상 유지.
> 통과 시 B안 확정, 미달 시 A안(클라이언트 직수신) 전환 검토.

RTSP를 TCP로 받으려면(패킷 유실 시 화면 깨짐 방지):

```bash
OPENCV_FFMPEG_CAPTURE_OPTIONS="rtsp_transport;tcp" make run
```

## 현재 구조

```
server/
 ├─ Makefile
 ├─ src/
 │   ├─ main.cpp        # 진입점 — 수신→리사이즈→JPEG→송출 파이프라인 조립
 │   └─ system_stats.*  # CPU 사용률·SoC 온도 측정 (벤치마크용)
 ├─ video/
 │   ├─ rtsp_client.*   # 채널 1개 수신·디코딩 워커 (자동 재연결)
 │   └─ frame_queue.hpp # 수신→처리 스레드 간 프레임 큐 (가득 차면 오래된 것부터 드롭)
 ├─ core/
 │   └─ stream_server.* # Qt 클라이언트로 JPEG 프레임 TCP 송출 (v1 평문, TLS 예정)
 └─ config/
     └─ cameras.conf.example  # 채널별 RTSP URL + 송출 포트
```

영상 패킷 형식은 [protocol/video_stream.h](../protocol/video_stream.h) 참조 (Qt 수신부 구현 기준).

### 클라이언트 없이 송출 확인하는 법

```bash
# 서버 실행 후 다른 터미널/PC에서 — 1초간 받은 바이트 수 확인
nc <서버IP> 5500 | head -c 100000 | xxd | head -5   # DB4B 매직 넘버가 보이면 정상
```
