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
sudo apt install -y g++ cmake libmosquitto-dev nlohmann-json3-dev
# SimpleBLE 는 소스 빌드 후 install

cmake -S . -B build && cmake --build build
./build/relay_node
```

`src/cfg` 네임스페이스에 HM-10 MAC(`TARGET_ADDR`), 브로커 주소, 토픽이 하드코딩돼
있다. 기기가 바뀌면 여기를 고친다.

## 알려진 제약

- **`temperature` 가 항상 0** — 펌웨어 `HM10_Send_Now()` 에서 `temp = 0` 더미로
  보낸다. `App/Algorithms/temperature_calc.c` 구현 후 연결해야 한다.
- **`WearableData` 사본** — `src/MqttClient_veda.*` 는
  `server/MQTT_dev/src/common/` 의 사본이고 `spo2` 필드가 추가돼 있다.
  서버 쪽 구조체엔 아직 `spo2` 가 없지만 nlohmann 이 모르는 키를 무시하므로
  master_node 파싱은 깨지지 않는다. 추후 구조체를 `protocol/` 로 옮겨
  공용화하면 이 사본은 삭제할 것.
