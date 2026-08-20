# protocol — 공통 통신 프로토콜

전 모듈(STM32 · 중계/알림 노드 · 서버 · 클라이언트)이 공유하는 패킷 정의입니다.

> ⚠️ **이 폴더의 변경은 전 모듈 재빌드를 유발합니다.**
> 변경 시 반드시 팀 전체 합의 → PR → 각 모듈 담당자 확인 순서로 진행하세요.

---

## 1. 통신 구간별 프로토콜

정의가 이 폴더에만 있는 게 아니라 **구간마다 주인이 다릅니다.** 아래 "정의 위치"가
그 구간의 유일한 규격 원본입니다.

| 구간 | 방식 | 정의 위치 |
| --- | --- | --- |
| 웨어러블 → 중계 노드 | **BLE** (HM-10 투과모드), 7바이트 바이너리 + XOR 체크섬, 1Hz | [`firmware/App/Drivers/hm10.h`](../firmware/App/Drivers/hm10.h) |
| 중계 노드 → 서버 | **MQTT/TLS** `veda/wearable/data` — `WearableData` JSON | [`MQTT/MQTT_prod/veda_messages.hpp`](../MQTT/MQTT_prod/veda_messages.hpp) |
| 서버·관제앱 → 알림 노드 | **MQTT/TLS** `veda/alarm/control` — `AlarmCommand` JSON | [`MQTT/MQTT_prod/veda_messages.hpp`](../MQTT/MQTT_prod/veda_messages.hpp) |
| 알림 노드 → 구독자 | **MQTT** `veda/alarm/<node_id>/status` — `online`/`offline` (retain + Last-Will) | [`client/mqttqtmanager.h`](../client/mqttqtmanager.h) |
| 카메라 → 서버 | RTSP (H.264 영상 + ONVIF 메타데이터 트랙 동시 수신) | [`docs/wiseai-메타데이터-명세.md`](../docs/wiseai-메타데이터-명세.md) |
| 서버 ↔ Qt (영상·이벤트·검색·제어) | TCP + **OpenSSL TLS**, 매직 넘버로 4종 구분 | **`video_stream.h`** (명세: `video_stream.md`) |

> 📌 **`protocol.h`는 현재 어느 모듈도 쓰지 않습니다.** 아래 4절 참조.

---

## 2. `video_stream.h` — 서버 ↔ Qt 관제

이 폴더에서 **실제로 쓰이는 유일한 헤더**입니다. 서버(`server/core/stream_server.*`,
`server/src/main.cpp`)와 Qt 클라이언트(`client/mainwindow.h`)가 같은 파일을 include 합니다.

### 하나의 TCP 연결, 양방향 4종

```
서버 → 클라  0xDB4B  dbj_vs_header_t (16B) + JPEG           영상 프레임
             0xDB4D  dbj_evt_header_t (18B)                 이벤트 (낙상·이탈·생체이상)
             0xDB4E  dbj_search_result_header_t (8B) + UTF-8 영상검색 결과 (요청 클라에게만)

클라 → 서버  0xDB4C  dbj_ctrl_header_t (8B) [+ 추가 구조체]  제어 메시지 10종
```

**모든 다바이트 필드는 리틀엔디언.** TCP가 무결성을 보장하므로 CRC는 없습니다.
매직 넘버가 안 맞으면 스트림이 어긋난 것이므로 연결을 끊고 재접속합니다.

### 제어 메시지 (클라 → 서버)

| 타입 | 값 | 헤더 뒤에 오는 것 |
| --- | --- | --- |
| `ROI_SET` | 0x01 | `dbj_roi_point_t` × `point_count` — 침대 ROI 다각형 |
| `ROI_CLEAR` | 0x02 | 없음 (`roi_id`=`DBJ_ROI_ID_ALL`이면 채널 전체 삭제) |
| `ALARM_CONFIRM` | 0x03 | 없음 — 경보 확인 → 블러 원복 |
| `RISK_UPDATE` | 0x04 | 없음 (`point_count`에 위험도 1~3) |
| `CAMERA_SET` | 0x05 | RTSP URL 문자열 (`reserved` = 길이) |
| `CAMERA_CLEAR` | 0x06 | 없음 |
| `IMAGE_SET` | 0x07 | `dbj_image_params_t` — 밝기/대비/채도 0~100 |
| `FOCUS_SET` | 0x08 | `dbj_focus_t` — 전체 또는 클릭 지점 영역 초점 |
| `ROI_BIND` | 0x09 | `dbj_roi_bind_t` — 침대 ↔ 입소자 매핑 |
| `SEARCH_QUERY` | 0x0A | UTF-8 질의 문자열 (`reserved` = 길이) |

### 좌표계

모든 좌표는 **화면 대비 0~1 정규화값을 10000배한 고정소수(uint16)** 입니다
(`DBJ_ROI_COORD_SCALE`). 서버의 `Detection.cx/cy`와 같은 좌표계라 그대로 점-다각형
판정에 넣을 수 있습니다. `(0,0)` = 좌상단, `(1,1)` = 우하단.

### 다중 침대 ROI

한 채널(= 한 병실 시야)에 침대가 여러 개이므로 ROI도 채널당 여러 개입니다
(최대 `DBJ_ROI_MAX_ZONES` = 8개, `roi_id`로 구분).

`roi_id`는 헤더 크기를 안 늘리려고 **기존 `reserved`의 하위 8비트를 재해석**해
실어 보냅니다(`ROI_SET` / `ROI_CLEAR` / `RISK_UPDATE`). 예전 클라이언트가 보내던
`reserved=0`이 자연히 `roi_id=0`("첫 번째 침대")이 되어 하위호환됩니다.
`CAMERA_SET`·`SEARCH_QUERY`만은 예전대로 `reserved` 전체가 문자열 길이입니다.

### 특수값

| 매크로 | 값 | 의미 |
| --- | --- | --- |
| `DBJ_ROI_ID_ALL` | 0xFF | 그 채널의 침대 전부 (`ROI_CLEAR` · `RISK_UPDATE`) |
| `DBJ_ROI_ID_NONE` | 0xFF | 이벤트 헤더에서 "발생 침대 미상" |
| `DBJ_CHANNEL_ALL` | 0xFF | `SEARCH_QUERY`의 전체 채널 검색 (**관제 Qt 전용**) |

> ⚠️ `DBJ_CHANNEL_ALL`은 보호자용(텔레그램 케어봇) 경로에서 쓰면 안 됩니다.
> 다른 방 사생활이 노출됩니다 — 케어봇은 항상 자기 채널만 조회합니다.

### TLS

서버 `config/cameras.conf`에 `stream_cert_path`/`stream_key_path`를 둘 다 주면
TLS로 뜨고, 비우면 평문입니다. **한쪽만 채우면 시작을 거부합니다** — "TLS를 켰다고
설정했는데 조용히 평문으로 내려가는" 사고를 막기 위한 것입니다.
프레임 포맷 자체는 TLS 유무와 무관합니다(전송 계층만 다름).

인증서 발급은 [`MQTT/scripts/generate_stream_certs.sh`](../MQTT/scripts/generate_stream_certs.sh)
— MQTT용 `DavoCA`를 재사용하므로 클라이언트의 `certs/ca.crt`는 재배포가 필요 없습니다.

---

## 3. MQTT JSON 계약

Pi 노드들은 `libmosquitto`, Qt 관제 앱은 `QMqttClient`로 붙어 **라이브러리가 다릅니다.**
주고받는 JSON만 같으면 되므로 그 계약만 [`veda_messages.hpp`](../MQTT/MQTT_prod/veda_messages.hpp)에
모아 두었습니다.

```jsonc
// veda/wearable/data — WearableData
{"device_id":"wearable_01","is_fall_detected":false,
 "heart_rate":78,"spo2":98,"steps":1000,"timestamp":1754000000000}

// veda/alarm/control — AlarmCommand
{"target_device":"alarm_rpi_01","room":301,"type":"FALL","message":"301호 낙상 발생",
 "audio_action":"PLAY","audio_file":"fall_alert.wav","volume":80,"loop":false,
 "matrix_action":"SHOW","matrix_passes":3,"brightness":200,
 "timestamp":1754000000000,"is_test":false}
```

| 종류 | QoS | 이유 |
| --- | --- | --- |
| 바이탈 (`is_fall_detected: false`) | 0 | 빠르게, 유실 감수 |
| 낙상 (`is_fall_detected: true`) | 1 | 반드시 전달 |

**`is_test`만 관대하게 파싱합니다** — 이 필드를 모르는(재빌드 전) 발신자가 보낸 명령도
`false`(= 실제 알람)로 안전하게 취급되도록 `j.value()`를 씁니다. 나머지 필드는
하나라도 빠지면 파싱 실패(의도한 동작).

> ⚠️ 토픽 문자열이 한 글자라도 어긋나면 **에러 없이 조용히 아무 일도 안 일어납니다.**
> 그래서 `MqttQtManager`의 상수 한 곳에서만 정의하고 나머지는 그걸 씁니다.

---

## 4. `protocol.h` — 채택되지 않은 초기 설계 ⚠️

**현재 어느 모듈도 include 하지 않습니다.** 프로젝트 초기에 "STM32 → 중계 노드는 UART,
중계 노드 → 서버는 Wi-Fi/TCP"를 전제로 설계한 프레임 규격인데, 이후 구현이 아래처럼
바뀌면서 쓰이지 않게 되었습니다.

| `protocol.h`의 전제 | 실제 구현 |
| --- | --- |
| UART 프레임 + CRC16-CCITT | **BLE** 7바이트 + XOR 체크섬 (`hm10.h`) |
| 중계 → 서버 Wi-Fi/TCP 캡슐화 | **MQTT/TLS** JSON (`veda_messages.hpp`) |
| 토픽 `dbj/alert/%d/cmd` | `veda/alarm/control` |
| `skin_temp_x100` (체온) | 측정 하드웨어가 없어 전 구간에서 제거 |
| `battery_pct`, `seq`, ACK/NACK | 미구현 |

> **정리 필요**: 이 파일을 지우거나 "미채택" 표기를 헤더 주석에 남기는 편이 좋습니다.
> 지금 상태로는 새로 합류한 사람이 이 규격이 살아 있다고 오해합니다.
> (`client/mqttqtmanager.h`에도 "현재 아무 모듈도 쓰지 않는다"는 주석이 있습니다.)

---

## 5. 서버 내부 데이터 계약 (video → core)

모듈 간 계약은 [`server/video/detection.hpp`](../server/video/detection.hpp)의
`Detection` 구조체입니다. 좌표는 전부 0~1 정규화.

| 필드 | 설명 |
| --- | --- |
| `object_id` | WiseAI 추적 키 (프레임 간 동일 객체 — **신원은 아님**) |
| `parent_id` | 이 객체가 속한 부모 ObjectId. `Head`는 자기 `Human`을 가리킴 |
| `left/top/right/bottom` | 바운딩 박스 |
| `cx, cy` | 무게중심 (`tt:CenterOfGravity`) |
| `likelihood` | 분류 확신도 0~1 (화면 가장자리에선 0.3까지 떨어짐) |
| `type` | `"Human"` \| `"Head"` \| `"Other"` |

> 카메라 자체 Slip&Fall 플래그는 **오지 않습니다** — 이 펌웨어에 해당 이벤트가 없어
> 서버가 MoveNet 자세 추정으로 직접 판정합니다. 상세는
> [`docs/wiseai-메타데이터-명세.md`](../docs/wiseai-메타데이터-명세.md).
