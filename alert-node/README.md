# 🔔 alert-node — 알림 노드 (Raspberry Pi 4)

**MQTT 경보를 받아 사이렌과 HUB75 LED 패널로 현장에 알리는 노드**

> 요양원 복도에 걸어 두는 경보기입니다. 서버가 낙상이나 침상 이탈을 확정하면 MQTT 로
> 명령이 날아오고, 이 노드가 사이렌을 울리면서 LED 패널에 `301호 홍O준 낙상 발생`
> 같은 문구를 흘립니다. **관제 앱에서 "경보 해제" 를 누를 때까지 소리와 화면이 유지됩니다.**

- **담당**: 김예훈, 이교민, 전승현

---

## 1. 하는 일

```
서버 ──MQTT(veda/alarm/control)──> RPi4 (alert-node) ──┬──> 스피커 (사이렌)
                                                       └──> 64x32 LED 패널 (문구)
```

경보가 없는 평상시에는 패널에 **현재 시각**을 띄웁니다.

관제 앱의 마이크 방송은 서버를 거치지 않고 **UDP로 이 노드에 직접** 옵니다. MQTT
`MIC_ON`/`MIC_OFF` 로 켜고 끄며, 켜져 있는 동안은 사이렌/WAV 대신 마이크가 나갑니다
(6절).

두 부품의 커널 드라이버를 **직접 구현**했습니다(프로젝트 필수 요구사항이라 기성
드라이버를 쓰지 않았습니다).

| 부품 | 무엇인가 | 특이점 |
| --- | --- | --- |
| **WM8960** | 오디오 코덱 칩 | 라즈베리파이가 I2C로 제어하고 I2S 로 보낸 디지털 소리를 아날로그로 바꿔 스피커로 출력.  |
| **HUB75** | LED 패널 배선 규격 | 패널이 화면을 스스로 기억하지 못합니다. 한 번에 두 줄만 켤 수 있어 CPU 가 32줄을 쉬지 않고 돌며 켜야 정지 화면으로 보입니다. 그래서 **코어 하나를 통째로** 이 일에 씁니다 |

---

## 2. 폴더 구조

```
drivers/wm8960    오디오 코덱 드라이버 (커널 모듈 + 오버레이)
drivers/hub75     64x32 LED 패널 드라이버 (커널 모듈 + 오버레이)
app/main.cpp      알림 노드 본체 (MQTT → 오디오 + 패널)
app/AudioPlayer   ALSA 재생 (VedaAudioPlayer)
app/matrix        패널 렌더러 (AlertDisplay) + 글꼴/글꼴 생성기
app/streaming     관제 앱 마이크 방송 UDP 수신 (AudioReceiver)
app/sounds        경보 음원
scripts/          모듈 적재/해제 + systemd 유닛
```

> **오버레이**(device tree overlay)는 커널에게 "이 GPIO 핀들에 이런 장치가 붙어
> 있다" 고 알려 주는 설정 조각입니다. 파이는 부팅할 때 이 정보를 읽어 장치 목록을
> 만들고, 그래야 우리가 만든 드라이버가 자기 장치를 찾아 붙습니다. 드라이버(`.ko`)와
> 오버레이(`.dtbo`)가 항상 짝으로 따라다니는 이유입니다.

---

## 3. 처음 한 번 해야 하는 것

새 파이에 올릴 때만 하면 됩니다. **현재 개발용 파이에는 이미 적용돼 있으니**,
아래로 확인해서 값이 나오면 이 절을 건너뛰고 4절로 갑니다.

```bash
grep -E "hub75|audio|spi=off|gpio=12" /boot/config.txt
grep -o "isolcpus=[^ ]*" /boot/cmdline.txt
```

### 부트 설정

패널에 필요한 것들이 전부 부팅 시점에 결정되는 값이라, 여기에 넣고 **재부팅**해야
합니다.

```
# /boot/config.txt
dtoverlay=hub75
gpio=12=op,dh,pu
dtparam=spi=off
dtparam=audio=off

# /boot/cmdline.txt (한 줄에 이어서)
isolcpus=3 irqaffinity=0,1,2
```

| 설정 | 왜 필요한가 |
| --- | --- |
| `isolcpus=3` | 리눅스에게 "CPU 3번은 쓰지 마라" 고 알립니다. 화면 갱신을 그 코어가 전담하는데 다른 작업이 끼어들면 화면이 떨립니다. **커널 부팅 파라미터라 재부팅 없이는 못 바꿉니다** |
| `gpio=12=op,dh,pu` | 패널 OE(출력 활성) 핀을 부팅하자마자 꺼 둡니다. 없으면 전원이 들어오고 드라이버가 올라오기 전까지 패널이 켜진 채로 있어 **뜨거워집니다** |
| `dtparam=spi=off` | SPI 기능이 GPIO 7~11 을 먼저 차지하는데, 그 핀들이 패널 데이터선입니다 |
| `dtparam=audio=off` | 파이 내장 아날로그 오디오가 패널과 같은 PWM 을 씁니다 |

### pulseaudio 가 사운드카드를 못 잡게 하기

데스크톱 화면이 뜨는 파이에서만 필요합니다. pulseaudio 가 사운드카드를 먼저 잡아
버리면 모듈을 다시 올린 직후 5~10초 동안 재생이 `Device or resource busy` 로
실패합니다. lightdm 과 사용자 세션이 각각 띄우기 때문에 두 번 겹치면 10초까지 갑니다.

udev 규칙으로 **이 카드만** 제외합니다. HDMI 오디오는 건드리지 않습니다.

```
# /etc/udev/rules.d/91-pulseaudio-ignore-wm8960.rules
SUBSYSTEM!="sound", GOTO="pa_end"
KERNEL!="card*", GOTO="pa_end"
ATTRS{id}=="WM8960CustomSou", ENV{PULSE_IGNORE}="1"
LABEL="pa_end"
```

```bash
sudo udevadm control --reload-rules
sudo ./scripts/unload.sh && sudo ./scripts/load.sh
```

---

## 4. 빌드 · 실행

빌드 의존성은 `raspberrypi-kernel-headers`, `libasound2-dev`, `libmosquitto-dev`,
`nlohmann-json3-dev` 입니다.

```bash
make                      # .ko 2개 + .dtbo 2개 + 유저 앱

sudo ./scripts/load.sh    # 오버레이 + 모듈 적재
sudo ./app/alert-node     # 본체 실행 (Ctrl-C 로 종료)

sudo ./scripts/unload.sh  # 모듈 해제
```

`load.sh` 는 이미 올라와 있으면 건너뛰므로 여러 번 실행해도 됩니다. 앱은 **root 로
띄워야 합니다** — 패널 밝기를 sysfs 로 바꾸기 때문입니다.

### 부팅할 때 모듈 자동 적재

이 유닛을 켜 두면 재부팅한 뒤 `load.sh` 없이 바로 앱만 띄우면 됩니다.

```bash
sudo cp scripts/alert-node-drivers.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now alert-node-drivers

systemctl status alert-node-drivers    # 확인
```

| 유닛 | 하는 일 | 기본값 |
| --- | --- | --- |
| `alert-node-drivers.service` | 모듈만 적재 | **켬** |
| `alert-node.service` | 모듈 적재 + 본체까지 실행 | **끔** |

> 본체 유닛을 기본으로 켜지 않는 이유는, 브로커 주소가 DHCP 로 계속 바뀌는 동안
> 주소가 틀리면 앱이 종료하고 systemd 가 5초마다 다시 띄우는 일을 끝없이 반복하기
> 때문입니다. 주소가 안정되면(서버 파이를 테일스케일에 올려 이름으로 부르는 식)
> `sudo systemctl enable --now alert-node` 로 넘기면 됩니다.

두 유닛 모두 경로가 이 저장소를 체크아웃한 자리를 그대로 가리킵니다. 기기마다
위치가 다르면 `ExecStart` 줄을 고쳐야 합니다.

---

## 5. 설정

`app/alert-node.conf` 를 실행 파일과 같은 디렉터리에서 읽습니다. `-c 경로` 로 다른
파일을 지정할 수 있습니다. 설정이 없으면 기본값으로 뜨고 그 사실을 출력합니다.

```
broker_host = localhost        # 배치할 때 서버 IP 로
broker_port = 8883             # MQTTS — 평문으로 되돌리려면 1883 + ca_path 비우기
ca_path     = certs/ca.crt     # 브로커 검증용 CA, 비우면 평문. git 에 안 올라감
node_id     = alarm_rpi_01     # AlarmCommand.target_device 와 비교
topic       = veda/alarm/control
audio_dir   = sounds           # 서버가 파일명만 보낼 때 찾는 곳
idle_text   = 감시 중          # 평상시 문구 (64px 를 넘으면 잘림)
idle_mode   = clock            # clock = 현재 시각, text = 위 문구
matrix_passes = 3              # 테스트 표시에만 쓰임
matrix_brightness = 128        # 평상시 밝기 0~255
broadcast_port = 5000          # 관제 앱 마이크 방송 UDP 수신 포트
alsa_device = hw:0,0           # 마이크 방송 재생 장치
```

인증서는 저장소에 없으니 기기마다 직접 배치해야 합니다. **`node_id` 는 명령의
`target_device` 와 비교해서, 다르면 그 명령을 무시하는 데 씁니다.**

---

## 6. 서버가 보내는 명령

토픽 `veda/alarm/control` 로 `AlarmCommand` JSON 이 옵니다.

| 필드 | 노드가 하는 일 |
| --- | --- |
| `type` | FALL / EGRESS → **빨강**, VITAL_ABNORMAL → **주황**, 그 외 초록 |
| `room`, `name` | 있으면 노드가 문구를 조립, 비면 `message` 를 그대로 표시. `name` 은 서버가 가운데 글자를 `O` 로 가려 보낸 것 |
| `audio_action` | `PLAY` / `STOP` / `MIC_ON` / `MIC_OFF` (원격 방송 토글 — 켜져 있는 동안 `PLAY` 무시) |
| `audio_file` | 파일명 또는 절대 경로 (파일명만 오면 `audio_dir` 에서 찾음) |
| `volume`, `loop` | 음량과 반복 재생 |
| `matrix_action` | `SHOW` / `CLEAR` / `NONE` |
| `matrix_passes` | **테스트 표시에만** 쓰이는 스크롤 횟수 (1~10 으로 잘림). 실제 경보는 해제까지 흘려서 무관 |
| `brightness` | 0~255 |
| `is_test` | 관제 앱 "테스트" 표시. 보여준 뒤 평상시 밝기·음량으로 되돌립니다 |

문구는 노드가 짧게 조립합니다. 64px 화면을 한 바퀴 흘리는 데 문구 길이만큼 시간이
들어서, **바뀌는 정보인 호실을 맨 앞에** 두고 나머지는 최소한으로 줄였습니다
(`301호 낙상 발생`).

---

## 7. 화면이 동작하는 규칙

**무엇을 언제까지 띄울지는 서버가 아니라 노드가 정합니다.** 서버는 명령을 보내는
시점에 그 뒤로 무엇이 올지 모르기 때문입니다.

### 평상시

현재 시각을 **시:분:초**로 띄웁니다. 초가 바뀔 때만 다시 그립니다.
`idle_mode = text` 로 두면 대신 `idle_text` 문구가 뜹니다.

> 파이에는 시계를 유지하는 배터리가 없습니다. 그래서 부팅 직후의 시각은
> `fake-hwclock` 이 복원해 둔 지난번 종료 시각이고, 실제와 몇 시간까지 어긋날 수
> 있습니다. **겉보기에는 멀쩡한 시각이라 더 헷갈립니다.** 틀린 시계를 벽에 거느니
> 안 거는 게 낫다고 보고, systemd-timesyncd 가 `/run/systemd/timesync/synchronized`
> 를 만들기 전까지는 `idle_text` 로 물러납니다.

### 경보가 왔을 때

- FALL/EGRESS/VITAL_ABNORMAL 은 왼쪽에 **alert.gif**(경고 삼각형) 를 반복 재생하고
  오른쪽에 금색 문구를 흘립니다. 아이콘 등장 자체가 새 경보 신호라 테두리 깜빡임은
  없습니다.
- 실제 경보(FALL/EGRESS/VITAL_ABNORMAL)는 **해제할 때까지 계속** 흘립니다. 소리와
  같은 규칙입니다 — 몇 바퀴 돌고 저절로 사라지면 아무도 없는 사이에 난 경보는
  없었던 것과 같아집니다. 관제 앱 "테스트"(`is_test`)만 `matrix_passes` 만큼 돌고
  끝납니다.
- 화면을 되돌리는 건 관제 앱의 **"경보 해제"** 입니다. 같은 명령이 `audio_action`
  STOP 으로 소리를, `matrix_action` CLEAR 로 화면을 끕니다.
- 다음 경보가 오면 진행 중이던 화면을 **곧바로 덮어씁니다.** 등급에 따른 우선순위는
  두지 않습니다 — 언제나 최신입니다.

---

## 8. 드라이버를 따로 다룰 때

본체를 거치지 않고 각 드라이버만 확인하고 싶을 때 봅니다.

### 🔊 wm8960 (오디오)

```bash
cd drivers/wm8960
make

sudo dtoverlay wm8960_custum_daboyjo.dtbo
sudo modprobe regmap-i2c            # 드라이버가 이 심볼을 씀
sudo insmod wm8960_custum_daboyjo.ko

cd ../../app/AudioPlayer
./veda_player <음원파일경로>
```

오버레이를 런타임에 올릴 수 있어서 재부팅이 필요 없습니다.

### 💡 hub75 (LED 패널)

패널 쪽은 오버레이만으로 끝나지 않습니다. **3절의 부트 설정이 먼저** 들어가 있어야
하고, 그래서 오버레이도 `/boot/config.txt` 에 함께 넣어 부팅 때 올립니다.

```bash
sudo apt install raspberrypi-kernel-headers

cd drivers/hub75
make                              # hub75.ko 와 hub75.dtbo 둘 다 생성
sudo cp hub75.dtbo /boot/overlays/
sudo reboot                       # 부트 설정이나 오버레이를 바꿨을 때만
```

모듈만 올리고 내릴 때는 이걸 씁니다.

```bash
make reload      # 빌드 + fbcon 언바인드 + rmmod + insmod 를 한 번에
make unload      # 내릴 때
```

### 📺 matrix (패널 렌더러)

`AlertDisplay`(`alert-display.hpp`/`.cpp`)가 hub75 패널에 한글 경보·시계·낙상 아이콘을
그리는 실제 렌더러입니다. 별도 실행 파일이 아니라 `main.cpp` 가 직접
컴파일해 링크하는 소스라 `app/matrix` 자체에는 빌드 타겟이 없습니다.

픽셀과 vsync 를 모두 `/dev/fbN` 하나로 처리합니다 (`FBIO_WAITFORVSYNC`). 프레임은
오프스크린에 완성한 뒤 프레임 경계에서 한 번에 복사합니다. **단일 버퍼라 이 순서를
깨고 fb 에 직접 그리면 스크롤이 찢어집니다.**

---

## 9. 글꼴

`font16.h` 는 `gen_font.py` 가 나눔고딕Bold 에서 구워내는 **자동 생성 파일**입니다.
직접 고치지 말고 스크립트를 고쳐 다시 뽑으세요. 관제 앱(Qt)의 LED 미리보기도 include
경로로 이 파일을 그대로 봅니다 — **복사본을 두지 않습니다.**

```bash
python3 app/matrix/gen_font.py      # fonts-nanum 과 python3-pil 필요, 5초쯤
```

한글 완성형 **11172 자와 ASCII 를 전부** 담습니다(11267 글리프, 450KB). 예전에는
화면에 띄울 문구에 나오는 글자만 손으로 골라 51 자만 넣었는데, 입주자 이름은 DB 에서
오는 임의의 한글이라 목록에 없는 글자를 만나면 `drawText` 가 조용히 8px 를
건너뜁니다. 낙상 알림에서 이름 한 글자가 소리 없이 사라지는 셈이라 골라 담는 방식을
그만뒀습니다.

> 글꼴에 없는 글자는 지금도 빈칸으로 지나갑니다. 한글과 ASCII 밖(예: 라틴 확장 `í`)이
> 여기 해당하며, 눈에 띄는 네모로 그리는 편이 낫습니다 — **아직 안 했습니다.**

---

## 10. 트러블슈팅

| 증상 | 원인과 대처 |
| --- | --- |
| 패널이 켜진 채로 멈춤 | `sudo raspi-gpio set 12 op dh pu` 로 응급 소등 |
| `rmmod` 가 거부됨 | HDMI 가 없으면 이 패널이 `fb0` 이 되고 리눅스 콘솔(fbcon)이 거기 붙어 모듈을 쓰는 중으로 잡습니다. `make reload` 가 그 연결을 먼저 끊어 줍니다 |
| 재생이 `Device or resource busy` | pulseaudio 가 카드를 잡고 있습니다. 3절의 udev 규칙을 넣으세요 |
| 로그가 실시간으로 안 보임 | 파이프로 넘길 때 버퍼에 갇힙니다. `sudo stdbuf -oL ./app/alert-node \| tee /tmp/alert.log` |
| 패널을 못 찾음 | hub75 모듈이 올라와 있는지 (`lsmod \| grep hub75`), `/dev/fb*` 가 있는지 |
| 브로커에 못 붙고 계속 재시작 | `broker_host` 가 지금 브로커 주소인지. 노드는 최초 연결 실패 시 일부러 종료하고 systemd 가 다시 띄웁니다 |
