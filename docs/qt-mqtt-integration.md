# Qt 관제 앱 MQTT 연동

담당: 홍성준 | 모듈 위치: `client/mqttqtmanager.*`, `MQTT/MQTT_prod/veda_messages.hpp`

---

## 개요

Qt 관제 앱이 MQTT 브로커에 직접 붙어, **웨어러블 생체·낙상 데이터를 받고** 알림 노드에
**제어 명령을 보낸다.** 기존 영상 경로(TCP)와는 완전히 별개의 연결이다.

---

## 1. MQTT 원리

### 발행/구독 (Publish / Subscribe)

MQTT는 기기끼리 직접 연결하지 않는다. 가운데 **브로커**를 두고 오간다.

```
발행자 ──publish──> [브로커] ──> 구독자 A
                        └──────> 구독자 B
```

- **발행하는 쪽은 누가 받는지 모른다.** 아무도 안 받아도 에러가 아니고, 10명이 받아도 된다.
- 받는 쪽은 미리 **구독 신청**을 해둔다. 그러면 브로커가 알아서 배달한다.
- 기기가 늘어도 발행 코드는 안 바뀐다 → 노드를 붙이고 떼기가 쉽다.

### 토픽 (Topic)

편지의 주소에 해당하는 문자열. 슬래시로 계층을 나눈다. 미리 등록할 필요 없이 그냥 쓰면 생긴다.

```
veda/wearable/data     veda/alarm/control
```

⚠️ **토픽이 한 글자라도 어긋나면 에러 없이 조용히 아무 일도 안 일어난다.** MQTT에서 가장
찾기 힘든 버그의 원인이다. 그래서 코드에서 문자열을 직접 쓰지 않고 상수 한 곳에 모아둔다
(`MqttQtManager::kTopicWearable`, `kTopicAlarm`).

### 페이로드 (Payload)

MQTT는 **내용물이 뭔지 전혀 관심 없다.** 그냥 바이트 덩어리를 옮길 뿐이다. 따라서
"무엇을 어떤 형식으로 담을지"는 우리가 정해야 하고, 이 프로젝트는 **JSON**을 택했다.

### QoS (전달 보장 수준)

| QoS | 뜻 | 비고 |
|-----|-----|------|
| 0 | 던지고 잊는다 | 유실 가능. 주기 데이터에 적합 |
| 1 | 도착 확인까지 재전송 | 최소 1회 도착. **중복 가능** |
| 2 | 정확히 1회 | 느림. 이 프로젝트에선 미사용 |

### Clean Session — 재연결 시 구독이 사라진다

브로커는 연결이 끊기면 **"이 클라이언트가 뭘 구독했었는지"를 잊는다.** 그래서 붙을 때마다
매번 다시 구독해야 한다. 이걸 빼먹으면 **연결은 되는데 메시지가 하나도 안 오는** 상태가 된다.

---

## 2. 프로젝트 적용

### 두 개의 통신 세계

이 프로젝트에는 성격이 다른 통신이 두 갈래로 존재한다.

```
── 세계 ① 영상 (TCP 직통) ─────────────────────────────
  [CCTV] ──RTSP──> [server] ──0xDB4B 프레임──┐
                             ──0xDB4D 이벤트──┤
                                              ├─> [Qt 관제앱]
                             <─0xDB4C 제어────┘

── 세계 ② 웨어러블/알림 (MQTT) ────────────────────────
  [STM32] ──UART──> [중계 Pi] ──veda/wearable/data──┐
                                                     v
                                                 [브로커]
                                                   ^  │
                          [중앙 Pi] ───────────────┘  │ veda/alarm/control
                          (낙상 판단)                  v
                                                 [알림 Pi]
```

Qt 관제 앱은 원래 **세계 ①에만** 있었다. 이번 작업으로 **세계 ②에도 연결**했다.

### 사용 토픽

| 토픽 | 방향 | 페이로드 | Qt의 역할 | QoS |
|------|------|----------|-----------|-----|
| `veda/wearable/data` | 중계 Pi → 브로커 | `WearableData` | 구독 | 0 |
| `veda/alarm/control` | 중앙 Pi / Qt → 브로커 | `AlarmCommand` | **구독 + 발행** | 1 |

`veda/alarm/control`을 **보내면서 동시에 구독**하는 이유: 중앙 Pi가 자동으로 울린 알람도
화면에 띄우기 위해서다. 대신 **내가 보낸 명령이 나에게도 되돌아온다** — 로그에 중복으로
쌓지 않으려면 받는 쪽에서 걸러야 한다.

### ⚠️ 왜 Qt MQTT 인가 (libmosquitto 대신)

라즈베리파이 노드들은 `libmosquitto`를 쓰지만, **Qt 관제 앱만 Qt MQTT(`QMqttClient`)를 쓴다.**

| | libmosquitto | Qt MQTT |
|---|---|---|
| 윈도우 빌드 | 배포 바이너리가 MSVC용 → MinGW에서 못 씀 | 순수 Qt, 그냥 됨 |
| 배포 | `windeployqt`가 dll을 안 챙겨줌 | 자동 처리 |
| 스레드 | 자체 스레드 → Qt 위젯 접근에 다리 필요 | Qt 이벤트 루프 위에서 동작 |
| 자동 재연결 | 있음 | **없음 → 직접 구현** |
| 라이선스 | EPL/EDL | GPLv3 또는 상용 |  

**결정 근거: 관제 앱은 윈도우에서 돌고, 라즈베리파이 노드들은 리눅스에서 돈다.**
플랫폼이 다르므로 전송 라이브러리도 각자 편한 것을 쓴다.

⚠️ **대가:** Qt MQTT는 Qt 오픈소스 설치 관리자에 **포함되어 있지 않다.** `qtmqtt`를 소스에서
빌드해 Qt에 설치해야 `find_package(Qt6 ... Mqtt)`가 찾는다. 설치 절차는 `client/README.md` 참고.

---

## 3. 데이터 계약 — `veda_messages.hpp`

라이브러리는 갈라져도 **주고받는 JSON은 반드시 같아야 한다.** 그래서 구조체 정의만
전송 코드에서 떼어내 별도 헤더로 두고, 양쪽이 공유한다.

```
        veda_messages.hpp        ← mosquitto도 Qt도 필요 없음
            ↑           ↑
  MqttClient_veda.hpp   mqttqtmanager.h
  (라즈베리파이 / mosquitto)  (윈도우 / Qt MQTT)
```

### WearableData — `veda/wearable/data`

| 필드 | 타입 | 설명 |
|------|------|------|
| `device_id` | string | 웨어러블 기기 id (예: `wearable_rpi_01`) |
| `is_fall_detected` | bool | IMU 낙상 감지 여부 |
| `temperature` | double | 체온 |
| `heart_rate` | int | 심박수 (bpm) |
| `spo2` | int | 산소포화도 (%) |
| `timestamp` | long long | Unix epoch **밀리초** |

### AlarmCommand — `veda/alarm/control`

| 묶음 | 필드 | 설명 |
|------|------|------|
| 대상 | `target_device` | 알림 노드 id (예: `alarm_rpi_01`) |
| | `room` | 호실 |
| 이벤트 | `type` | `FALL` \| `EGRESS` \| `VITAL_ABNORMAL` \| `CONTROL` |
| | `message` | 로그 기록용 + 알림 노드 LED 표시용 (이중 용도) |
| 오디오 | `audio_action` | `PLAY` \| `STOP` |
| | `audio_file` | 재생할 wav 경로 |
| | `volume` | 음량 |
| | `loop` | 반복 여부 |
| LED | `matrix_action` | `SHOW` \| `CLEAR` \| `NONE` |
| | `matrix_passes` | 스크롤 반복 횟수 |
| | `brightness` | 0~255 |
| | `timestamp` | Unix epoch 밀리초 |

### JSON 직렬화

`nlohmann/json`의 매크로 한 줄이 변환 함수 두 개(`to_json` / `from_json`)를 자동 생성한다.

```cpp
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlarmCommand, target_device, room, ...)
```

⚠️ **구조체 필드 이름이 그대로 JSON 키가 된다.** 필드 이름을 바꾸면 컴파일은 멀쩡히 되고
런타임에 파싱이 실패한다. 필드를 추가할 때는 **구조체와 매크로 목록 둘 다** 고쳐야 한다.

⚠️ 매크로가 생성하는 `from_json`은 `j.at("키")`를 쓴다 — **필드가 하나라도 빠지면 예외를
던진다.** 그래서 수신 측은 반드시 try-catch로 감싼다.

---

## 4. 구현 — `MqttQtManager`

`QObject`를 상속한 다리 역할 클래스. MainWindow는 MQTT를 몰라도 되고, **시그널만 받아서
화면을 갱신**하면 된다.

### 공개 인터페이스

| 구분 | 이름 | 역할 |
|------|------|------|
| 함수 | `init(ip, port, id)` | 접속 시작. **비동기** — 반환값은 "요청을 걸었다"는 뜻 |
| | `disconnectFromBroker()` | 의도적 종료 (자동 재연결 중단) |
| | `isConnected()` | 현재 연결 상태 |
| 슬롯 | `sendAlarmCommand(cmd, qos)` | `AlarmCommand` → JSON → 발행 |
| | `sendAlarmClear(room, target_device, qos)` | 경보 해제 명령 조립 후 위 함수 호출 |
| 시그널 | `connected()` / `disconnected()` | 연결 상태 변화 |
| | `connectionError(msg)` | 사람이 읽을 문장으로 오류 전달 |
| | `wearableDataReceived(data)` | 생체·낙상 도착 |
| | `alarmCommandReceived(cmd)` | 알람 명령 도착 |
| | `payloadRejected(topic, why)` | JSON이 깨져서 버림 |

### 재연결 — 직접 구현

`QMqttClient`에는 자동 재연결이 없다. `QTimer`로 **5초마다** 재시도한다
(`kRetryIntervalMs`). 연결되면 타이머를 멈추고, 사용자가 의도적으로 끊은 경우
(`m_userDisconnect`)에는 재시도하지 않는다.

**연결될 때마다 `subscribeAll()`로 구독을 다시 건다** — clean session 때문에 필수다.

### 오프라인 큐를 두지 않은 이유

라즈베리파이 쪽 `MqttClient_veda`는 끊긴 동안의 발행을 큐에 쌓아 재연결 시 흘려보낸다.
**관제 앱은 일부러 그렇게 하지 않았다.**

> 관제사가 끊긴 줄 모르고 "경보 해제"를 누름 → 5분 뒤 재연결 → **그 사이 새로 발생한
> 알람을 꺼버림**

관제 앱에서는 큐잉이 기능이 아니라 위험이다. 실패를 알리는 편이 안전하다.

### ⚠️ 구현 중 걸렸던 함정 두 가지

**① `Q_DECLARE_METATYPE` 선언 순서**

`MainWindow`의 슬롯이 `WearableData`를 인자로 받는다. moc은 슬롯 인자마다 `QMetaTypeId`를
건드리는데, `Q_DECLARE_METATYPE`이 그보다 늦게 보이면 컴파일이 깨진다.

```
error: specialization of 'QMetaTypeId<WearableData>' after instantiation
```

→ `mainwindow.h`가 `mqttqtmanager.h`를 **통째로 include**해서 선언이 먼저 오게 했다.
전방 선언으로 가볍게 하려다 이 문제를 만났다.

**② 종료 시 크래시 (소멸 순서)**

```
~MainWindow 본문 실행 → MainWindow 부분은 이미 파괴됨
  → ~QObject 가 자식 삭제 → MqttQtManager 삭제
    → QMqttClient 가 끊기며 stateChanged 발사
      → emit disconnected() → MainWindow 슬롯 호출 → 💥
```

```
ASSERT: "Called object is not of the correct type
         (class destructor may have already run)"
```

→ `MqttQtManager` 소멸자에서 **정리 전에 양방향 연결을 끊는다**
(`m_client->disconnect(this)` + `blockSignals(true)`).

---

## 5. 화면 연동 — `MainWindow`

### 기기 id → 채널 매핑

브로커는 `"wearable_rpi_01"`이라는 **기기 id**로만 알려주는데, 화면은 **채널 번호(0~3)**로
구성돼 있다. 이 둘을 잇는 다리가 필요하다.

**`residents` 테이블이 그 다리다.** `loadPatientsFromDb()`에서 매핑을 만든다.

```sql
SELECT camera_id, name, room, wearable_id FROM residents
WHERE status='재원' AND camera_id BETWEEN 0 AND 3
```

→ `QHash<QString, int> wearableToChannel`

⚠️ **등록되지 않은 기기의 데이터는 버린다.** 엉뚱한 채널에 남의 심박수를 띄우느니 안 띄우는
편이 낫다. 이때 로그가 남는다:

```
[MQTT] 미등록 웨어러블 무시: "wearable_rpi_01"
       (residents.wearable_id 에 등록하면 해당 채널에 표시됩니다)
```

**화면에 값이 뜨려면 DB 관리 탭에서 입소자의 "웨어러블 ID" 칸을 채워야 한다.**
(`camera_id`가 0~3 범위로 배정돼 있고 `status`가 `재원`이어야 함)

### 수신 흐름

```
  [웨어러블] ──> [중계 Pi] ──publish──> [브로커]
                                          │
                                    QMqttClient
                                          │ messageReceived
                                MqttQtManager  (JSON → 구조체)
                                          │ emit wearableDataReceived
                                MainWindow::onWearableData
                                          │ wearableToChannel 조회
                                vitals_[ch] 저장 + 그래프 점 추가
                                          │
                                updateVitals() → 화면
```

`updateVitals()`는 **표시만** 담당한다. 값 저장과 그래프 점 추가는 데이터가 실제로 도착한
순간에만 한다 — 2초 타이머에서 `addValue`를 하면 새 값이 없어도 같은 값이 쌓여 실제 측정
간격이 그래프에서 사라진다.

### 값이 없을 때의 표시

**난수 시뮬레이션을 제거했다.** 낙상 관제 화면에서 그럴듯한 가짜 숫자는 관제사가 멀쩡한
줄 알게 만든다.

| 상태 | 표시 |
|------|------|
| 한 번도 못 받음 | `--` + **대기** 배지 |
| 받다가 30초 넘게 끊김 (`kVitalStaleMs`) | `--` + **신호 끊김** 배지 |
| 정상 수신 | 실제 값 + 정상/주의/위험 색 |

"한 번도 못 받음"과 "받다가 끊김"을 구분하는 이유: 전자는 등록·배선 문제, 후자는 기기가
빠졌거나 중계 노드가 죽은 것 — **대응이 다르다.**

### 발행 흐름 — 경보 해제

```
[관제사가 "경보 해제" 클릭]
            │
  MainWindow::onAlarmClearClicked()
            │
   ┌────────┴────────┐
   ↓                 ↓
(A) TCP 직통      (B) MQTT
   ↓                 ↓
server            [브로커] → [알림 Pi]
"모자이크 복구"     "소리 끄고 LED 지워"
  (0x03)
```

**두 경로는 다른 일을 한다.** A는 영상 모자이크 복구, B는 현장 사이렌·LED 정지.
하나만 보내면 화면은 정상인데 복도에서 사이렌이 계속 울리는 상황이 된다. 그래서 둘 다 보낸다.

### 브로커 주소 설정

`QSettings`에 저장한다. 관제 PC마다 다른 브로커를 볼 수 있다.

| 키 | 기본값 |
|----|--------|
| `mqtt/brokerHost` | `172.20.32.10` |
| `mqtt/brokerPort` | `1883` |

---

## 6. 알려진 한계 / 앞으로 할 일

| # | 내용 |
|---|------|
| 1 | **알림 노드가 `audio_action == "PLAY"`만 처리한다.** `STOP` 분기가 없어 경보 해제를 보내도 현장 소리가 실제로 꺼지지 않는다 (`MQTT/MQTT_dev/src/alarm_node/main_alarm.cpp`) |
| 2 | **채널이 4개로 고정돼 있다.** `patients[4]`, `vitals_[4]` 등 배열 크기와 `camera_id BETWEEN 0 AND 3` 쿼리, `kNumServers = 2` 가 하드코딩 — 5채널 이상은 코드 수정 필요 |
| 3 | **`protocol/protocol.h`의 MQTT 토픽 규격(`dbj/alert/...`)을 아무 모듈도 쓰지 않는다.** 실제 구현은 `veda/...` 를 쓴다. 어느 쪽으로 통일할지 팀 결정 필요 |
| 4 | 웨어러블 낙상(MQTT)과 카메라 낙상(TCP `0xDB4D`)이 **별개 경로로 들어온다.** 같은 사건이 화면에 두 번 뜰 수 있어 중복 제거 로직이 필요하다 (현재 로그만 남김) |
| 5 | `AlarmCommand`에 `channel` 필드가 없다. `room`(호실 문자열)만 있어 CCTV 채널과의 연결이 간접적이다 |

---

## 관련 문서 / 파일

| 항목 | 위치 |
|------|------|
| Qt MQTT 설치 절차 | `client/README.md` |
| Qt 측 구현 | `client/mqttqtmanager.h`, `client/mqttqtmanager.cpp` |
| 화면 연동 | `client/mainwindow.cpp` (`onWearableData`, `updateVitals`, `loadPatientsFromDb`) |
| 공유 데이터 계약 | `MQTT/MQTT_prod/veda_messages.hpp` |
| 라즈베리파이 측 MQTT 클라이언트 | `MQTT/MQTT_prod/MqttClient_veda.*` |
| 전체 프로토콜 규격 | `protocol/protocol.h`, `protocol/video_stream.h` |

---

## 요약

Qt 관제 앱은 영상용 TCP와 **완전히 별개인 MQTT 연결**로 웨어러블 생체·낙상을 받고 알림 노드에
제어 명령을 보낸다 — 토픽 문자열은 상수 한 곳에 모으고, 알람은 QoS 1로 보내고, 재연결할 때마다
다시 구독하는 것이 동작의 전제다.

경보 해제만은 TCP(모자이크 복구)와 MQTT(사이렌·LED 정지) **두 경로를 모두** 보내야 한다 —
하나만 보내면 화면은 멀쩡한데 복도에서 사이렌이 계속 울리는 상태가 된다.
