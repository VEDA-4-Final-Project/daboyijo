# relay-node — 중계 노드 (Raspberry Pi 4)

담당: 김예훈, 이교민, 전승현

STM32 웨어러블의 센서 데이터를 BLE로 받아 MQTT 브로커로 중계한다.

```
STM32 ──UART(9600)──> HM-10 ──BLE──> RPi4 (relay_node) ──MQTT──> 브로커 ──> master_node
```

- HM-10 BLE 연결·자동 재연결, FFE1 notify 구독
- 5바이트 바이너리 패킷 재조립·검증
- 네트워크 단절 대비 내부 큐 버퍼링 (재연결 시 자동 flush)
- 수신 시각 타임스탬프 부여 (패킷에 시각 필드 없음)

## 수신 패킷 (5바이트)

`firmware/App/Drivers/hm10.h` 와 동일 스펙. 체크섬 없음 — 필드 범위로 검증한다.

| 오프셋 | 필드 | 범위 |
| --- | --- | --- |
| 0 | 헤더 `0xAA` | 고정 |
| 1 | `heart_rate` (bpm) | 0~255 |
| 2 | `spo2` (%) | 0~100 |
| 3 | `temp` (정수 °C) | 0~100 |
| 4 | `fall_flag` | 0 / 1 |

펌웨어는 **1Hz 주기** 송신하고, 낙상 확정 시 주기를 기다리지 않고 즉시 1회 추가 송신한다.
`fall_flag` 는 엣지가 아니라 **레벨**이라 확정 후 5초간 1로 유지된다
(`HM10_FALL_HOLD_MS`). 그대로 흘리면 낙상 1건에 알람이 5~6번 뜨므로
게이트웨이가 **상승엣지(0→1)만** 낙상으로 발행한다.

## 발행 (MQTT)

토픽 `veda/wearable/data`, payload 는 `WearableData` JSON.

```json
{"device_id":"wearable_01","is_fall_detected":false,"temperature":0.0,
 "heart_rate":78,"spo2":98,"timestamp":1754000000000}
```

| 종류 | QoS |
| --- | --- |
| 바이탈 (`is_fall_detected: false`) | 0 — 빠르게, 유실 감수 |
| 낙상 (`is_fall_detected: true`) | 1 — 반드시 전달 |

## 빌드·실행

```bash
./install_deps.sh                      # 최초 1회
cmake -S . -B build && cmake --build build
./build/relay_node
```

`RELAY_DEBUG_HEX=1` 을 붙이면 BLE 로 들어온 원시 바이트를 hex 로 찍는다.
값이 이상할 때 파싱 전 단계를 확인하는 용도.

의존성은 `libmosquitto`, `nlohmann_json`, `SimpleBLE` 세 가지다. 앞의 둘은
apt 로 받고, SimpleBLE 는 apt 패키지가 없어서 `install_deps.sh` 가 소스에서
빌드해 `/usr/local` 에 설치한다. CMake 는 `find_package(simpleble)` 로 찾으므로
설치 후에는 소스 트리가 필요 없다.

HM-10 MAC(`TARGET_ADDR`), 브로커 주소, 토픽은 `src/main_relay.cpp` 상단에 상수로
하드코딩돼 있다. 기기가 바뀌면 여기를 고친다.

## MQTT 클라이언트

`MqttClient_veda` 는 이 디렉토리에 사본을 두지 않고 프로젝트 공용 구현인
`MQTT/MQTT_prod/` 를 CMake 에서 직접 참조한다. 따라서 `relay-node/` 만 따로
떼어내면 빌드되지 않고, 저장소 전체가 있어야 한다.

`WearableData` 는 전 노드가 공유하는 구조체다. 필드를 바꾸면 master/alarm/qt
노드의 JSON 계약이 함께 바뀌므로 `MQTT/MQTT_dev/src/common/` 쪽도 같이 맞춰야 한다.

## 알려진 제약

- **`temperature` 가 항상 0** — 펌웨어 `HM10_Send_Now()` 에서 `temp = 0` 더미로
  보낸다. `App/Algorithms/temperature_calc.c` 구현 후 연결해야 한다.
- **심박은 범위 검증을 하지 않는다** — 5바이트 패킷에 체크섬이 없어 필드 범위로
  헤더 오정렬을 걸러내는데, 심박은 0~255 전체가 유효값이라 검사할 수 없다.
  SpO2·체온·낙상 세 필드로만 판정한다.
