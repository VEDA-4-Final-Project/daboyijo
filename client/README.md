# client — Qt 관제 클라이언트

담당: 전체

요양원 관제실에서 쓰는 데스크톱 앱입니다. 두 대의 중앙 서버(2-Pi 분할)에 동시 접속해
4채널 영상을 받고, 경보를 띄우고, 기록을 조회하고, 카메라·알림 노드를 원격 제어합니다.

**Qt 6.5** (Widgets · Network · Sql · Multimedia · Mqtt), CMake 빌드, Windows/Linux.

---

## 화면 구성 (좌측 내비게이션 6개)

### 1. 실시간 관제
- 4분할 그리드 / 레이아웃 프리셋, 채널 확대·숨김, 리소스 트리(Root > 그룹 > 카메라 4대)
- **경보 배너·토스트** — 낙상·침상이탈·생체이상 발생 시 해당 채널 강조
- **타임라인 바** — 이벤트 마커 표시, 클릭으로 과거 구간 재생(playback) 전환·탐색
- **바이탈 패널** — 입소자별 심박·SpO2·걸음 수 타일(스파크라인 포함, MQTT 실시간 수신)
- 인터콤 방송(마이크 푸시투토크), 채널 스냅샷 저장

### 2. 이벤트 기록
- 낙상=빨강 / 침상이탈=주황, 미확인=빨강 / 확인=초록으로 상태 구분
- 날짜·유형 필터, 확인 처리
- 행 클릭 → **블랙박스 클립 재생** / **NVR 구간 점프** / **클립 다운로드**

### 3. 영상 검색
- 자연어 질의를 서버로 보내(`SEARCH_QUERY`) Gemini가 구조화한 결과를 회신받습니다
- 전체 채널 검색 가능(`DBJ_CHANNEL_ALL`) — DB를 두 Pi가 공유하므로 아무 Pi에나 물어봐도 전체 결과가 나옵니다

### 4. 일일 리포트
- 날짜 + 입소자 **한 명 단위**. 상단 이름 탭으로 전환
- 재실 시간 · 케어 시간 · 이벤트 집계 지표 타일
- **24시간 활동량 그래프** (`activity_minute` 집계)
- **PDF 내보내기** (QTextDocument → QPdfWriter)
- **AI 요약** — 집계 수치만 Gemini로 보냅니다. 원본 로그(개별 이벤트 행, 분당 걸음)는 보내지 않습니다

### 5. 입소자 관리
- 등록 · 수정 · 퇴원 · 재입원, 위험도(상/중/하) 설정, 변경 이력 기록
- 위험도는 `residents.risk_level`에 Qt가 직접 기록하고, 서버가 부팅 시 읽어 복원합니다

### 6. 장치 설정 — [카메라] / [알림] 서브탭
**카메라**
- 채널 레일(CH1~4) — 연결 상태·지정된 침대 수 배지
- **연결** — CCTV IP·계정 입력 또는 "같은 망 카메라 검색"으로 자동 탐색 → 서버가 그 RTSP를 엽니다
- **ROI(침대)** — 영상 위에 직접 클릭해 침대 영역을 그리고 더블클릭으로 완료. 채널당 최대 8개
- **침대 ↔ 입소자 매핑** — 그 침대에서 감지된 사람에게 이름이 붙어 알림에 "침대2 김복순"으로 표시됩니다. 서버가 특정 못 하면 "신원 미상"
- **이미지** — 밝기·대비·채도 슬라이더(ONVIF), 적용 전/후 실시간 비교. 영상 클릭 시 그 지점 초점(SUNAPI)

**알림 노드**
- LED 밝기·스피커 음량 조절, **HUB75 미리보기**(실제 표시와 같은 폰트)
- "테스트"는 현장 LED에 문구 1회 + 짧은 소리, "적용"은 평상시 설정으로 저장
- 노드 온라인 상태 표시(`veda/alarm/+/status` retain + Last-Will)
- 경보 원격 해제

### 공통
- **로그인 / 회원가입** — `PBKDF2-HMAC-SHA256` 비밀번호 해싱(계정별 salt, 반복 횟수 컬럼 보관).
  최초 실행 시 `users` 테이블이 없으면 만들고 초기 관리자(`admin`/`admin1234`)를 넣습니다
- 다크/라이트 테마, Windows 네이티브 다크 타이틀바
- 도움말 패널 (화면별 사용법)

---

## 연결 구성

| 대상 | 방식 | 기본값 |
| --- | --- | --- |
| 영상 서버 A (ch0·1) | `QSslSocket` TCP 5500 | `QSettings: server/hostA` |
| 영상 서버 B (ch2·3) | `QSslSocket` TCP 5500 | `QSettings: server/hostB` |
| MQTT 브로커 | `QMqttClient` **8883 (MQTTS)** | `QSettings: mqtt/brokerHost` |
| MariaDB | `QMARIADB` 3306 | `client/main.cpp` |

- **TLS**: `certs/ca.crt`(MQTT용과 같은 파일) 유무로 TLS 여부를 스스로 판단합니다.
  서버 인증서 CN은 `DaboStreamA` / `DaboStreamB`, 브로커는 `DaboBroker` —
  `MQTT/scripts/generate_stream_certs.sh`의 CN과 반드시 일치해야 합니다
- **DB는 조회 전용**입니다. 이벤트·케어로그·재실 기록은 전부 서버가 씁니다 —
  관제 PC는 퇴근하면 꺼지는데 낙상·침상이탈은 밤에 제일 많아서, Qt가 쓰면
  리포트의 밤 시간대가 통째로 빕니다

---

## 빌드

Qt Creator에서 `client/CMakeLists.txt`를 엽니다.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt6 경로>
cmake --build build
```

**AI 요약 기능**을 쓰려면 빌드 시 Gemini 키를 지정해야 합니다.

```bash
cmake -S . -B build -DDABOYIJO_GEMINI_KEY=<발급받은 키>
```

(Qt Creator: 프로젝트 > 빌드 설정 > CMake)
키가 없으면 AI 요약 버튼만 안내 메시지를 띄우고 **나머지 리포트 기능은 그대로 동작**합니다.

---

## 참고

- 서버와 주고받는 패킷 규격: [`protocol/video_stream.h`](../protocol/video_stream.h)
- MQTT JSON 계약: [`MQTT/MQTT_prod/veda_messages.hpp`](../MQTT/MQTT_prod/veda_messages.hpp)
- 토픽 문자열은 `mqttqtmanager.h`의 상수 한 곳에서만 정의합니다 —
  한 글자라도 어긋나면 **에러 없이 조용히 아무 일도 안 일어납니다**
- LED 미리보기는 `font16.h`에 있는 글자만 보입니다(숫자·"확인"은 있음).
  알림 노드와 같은 폰트 파일을 씁니다
