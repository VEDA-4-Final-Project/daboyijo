#!/usr/bin/env python3
"""웨어러블 5대 더미 발행기 — veda/wearable/data 로 '정상' 생체값을 계속 쏜다.

관제 앱(client/) 바이탈 카드는 MQTT 로 실제로 들어온 값만 그린다
(mainwindow.cpp onWearableData) — 웨어러블/릴레이 노드가 없는 자리에서
화면·색·스파크라인을 확인하려면 이 스크립트가 그 자리를 대신한다.

  * JSON 규격은 MQTT/MQTT_prod/veda_messages.hpp 의 WearableData 와 같아야 한다.
    nlohmann 의 NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE 는 필드가 하나라도 빠지면
    파싱이 통째로 실패하므로 6개(device_id/is_fall_detected/heart_rate/spo2/
    steps/timestamp)를 항상 채워 보낸다.
  * device_id 는 residents.wearable_id 와 글자까지 같아야 한다. 관제 앱은
    등록되지 않은 기기의 값을 "미등록 웨어러블 무시" 로 버린다.
        UPDATE residents SET wearable_id='wearable_01' WHERE resident_id=1;
    (status='재원' + camera_id 0~3 인 입소자만 카드가 생긴다)
  * 값은 client/mainwindow.cpp vitalLevel() 기준 '정상' 대역 안에서만 흔들린다
    — SpO2 96~99(<95 면 주의), 심박 58~95(>=100 주의, >=110 위험).
  * 발행 간격 기본 2초. 30초 넘게 끊기면 카드가 "신호 끊김" 으로 바뀐다
    (kVitalStaleMs) — 간격을 늘릴 때 이 선을 넘기지 말 것.

사용 예:
    python MQTT/scripts/dummy_vitals.py                       # TLS 8883, 5대
    python MQTT/scripts/dummy_vitals.py --no-tls              # 평문 1883
    python MQTT/scripts/dummy_vitals.py --ids w1,w2,w3,w4,w5  # 기기 id 직접 지정
"""

import argparse
import json
import random
import ssl
import sys
import time
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt 가 없습니다 —  python -m pip install paho-mqtt")

# 관제 앱 기본값과 같은 자리(client/mainwindow.cpp brokerHost/brokerPort).
DEFAULT_HOST = "172.20.32.51"
DEFAULT_TLS_PORT = 8883
DEFAULT_PLAIN_PORT = 1883
TOPIC = "veda/wearable/data"

# 릴레이 노드(relay-node/src/main_relay.cpp)의 device_id 표기를 그대로 따른다.
DEFAULT_IDS = [f"wearable_{i:02d}" for i in range(1, 6)]

# 기본 CA — 관제 앱이 쓰는 파일과 같은 것(client/certs/ca.crt).
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CA = REPO_ROOT / "client" / "certs" / "ca.crt"

# 정상 대역. vitalLevel() 의 경계(95 / 55 / 100)에 딱 붙지 않게 한 칸씩 안으로 뒀다 —
# 경계값을 오가면 데모 중에 배지가 정상/주의로 깜빡인다.
SPO2_RANGE = (96, 99)
HR_RANGE = (58, 95)


class Device:
    """기기 1대. 심박·SpO2 를 난수 재추첨이 아니라 임의보행으로 흔든다 —
    매번 새로 뽑으면 스파크라인이 정상 대역 안에서 톱니처럼 튄다."""

    def __init__(self, device_id: str, hr_base: int):
        self.device_id = device_id
        self.hr = hr_base
        self.spo2 = random.randint(97, 99)
        self.steps = random.randint(200, 3000)   # 만보기는 누적값이라 0부터 시작하지 않는다

    def tick(self) -> dict:
        self.hr = _clamp(self.hr + random.randint(-2, 2), *HR_RANGE)
        self.spo2 = _clamp(self.spo2 + random.choice((-1, 0, 0, 1)), *SPO2_RANGE)
        self.steps += random.randint(0, 4)
        return {
            "device_id": self.device_id,
            "is_fall_detected": False,      # 정상 데이터만 — 낙상 경보는 울리지 않는다
            "heart_rate": self.hr,
            "spo2": self.spo2,
            "steps": self.steps,
            "timestamp": int(time.time() * 1000),
        }


def _clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


def main() -> int:
    p = argparse.ArgumentParser(description="웨어러블 더미 생체값 발행기(정상값)")
    p.add_argument("--host", default=DEFAULT_HOST, help=f"브로커 주소 (기본 {DEFAULT_HOST})")
    p.add_argument("--port", type=int, help="브로커 포트 (기본 TLS 8883 / --no-tls 면 1883)")
    p.add_argument("--ids", default=",".join(DEFAULT_IDS),
                   help="쉼표로 구분한 device_id 목록 (residents.wearable_id 와 같아야 함)")
    p.add_argument("--interval", type=float, default=2.0, help="발행 간격(초, 기본 2)")
    p.add_argument("--count", type=int, default=0, help="기기당 발행 횟수 (0=무한)")
    p.add_argument("--no-tls", action="store_true", help="평문(1883)으로 접속")
    p.add_argument("--ca", default=str(DEFAULT_CA), help=f"CA 인증서 (기본 {DEFAULT_CA})")
    args = p.parse_args()

    # 윈도우 콘솔(cp949)은 em dash 같은 글자에서 UnicodeEncodeError 로 죽는다.
    # 로그 한 줄 때문에 발행기가 멈추면 안 되니 못 찍는 글자는 대체 문자로 흘린다.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass

    ids = [s.strip() for s in args.ids.split(",") if s.strip()]
    if not ids:
        return p.error("--ids 가 비어 있습니다")
    port = args.port or (DEFAULT_PLAIN_PORT if args.no_tls else DEFAULT_TLS_PORT)

    # 기기마다 다른 기준 심박 — 카드 5장이 똑같은 그래프를 그리면 라우팅이 맞는지
    # (누구 값이 누구 카드로 갔는지) 눈으로 확인할 수 없다.
    bases = [62, 70, 78, 66, 85]
    devices = [Device(d, bases[i % len(bases)]) for i, d in enumerate(ids)]

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"dummy_vitals_{random.randint(1000, 9999)}")
    if not args.no_tls:
        ca = Path(args.ca)
        if not ca.is_file():
            return p.error(f"CA 인증서를 못 찾음: {ca} (--ca 로 지정하거나 --no-tls 사용)")
        client.tls_set(ca_certs=str(ca), cert_reqs=ssl.CERT_REQUIRED)
        # 호스트명 검증만 끈다 — 관제 앱도 같은 예외를 둔다(mqttqtmanager.cpp).
        # 브로커 인증서가 IP/호스트명 여러 개로 쓰이는 사내망 구성 때문이다.
        client.tls_insecure_set(True)

    print(f"[dummy] 접속 {args.host}:{port} "
          f"({'평문' if args.no_tls else 'TLS'}) / 기기 {len(devices)}대 / {args.interval}초 간격")
    try:
        client.connect(args.host, port, keepalive=30)
    except Exception as e:                       # 브로커 다운·방화벽·CA 불일치 모두 여기로
        return f"[dummy] 접속 실패: {e}"
    client.loop_start()

    sent = 0
    try:
        while args.count == 0 or sent < args.count:
            for dev in devices:
                payload = json.dumps(dev.tick())
                client.publish(TOPIC, payload, qos=0)   # 주기 데이터 — 앱도 QoS 0 으로 받는다
                print(f"[dummy] {payload}")
            sent += 1
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[dummy] 종료")
    finally:
        client.loop_stop()
        client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
