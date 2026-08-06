# alert-node — 알림 노드 (Raspberry Pi 4)

담당: 김예훈, 이교민, 전승현

- MQTT 구독(`veda/alarm/control`) → 낙상 확정 시 오디오 사이렌 + LED 경보
- 오디오/LED 디바이스 드라이버 직접 구현 (프로젝트 필수 요구사항)
- 클라이언트發 해제 신호 처리


구성

```
drivers/wm8960    오디오 코덱 드라이버 (커널 모듈 + 오버레이)
drivers/hub75     64x32 LED 패널 드라이버 (커널 모듈 + 오버레이)
app/main.cpp      알림 노드 본체 (MQTT → 오디오 + 패널)
app/AudioPlayer   ALSA 재생 (VedaAudioPlayer)
app/matrix        패널 렌더러 (AlertDisplay) + 데모
app/sounds        경보 음원
scripts/          모듈 적재/해제
```

빌드 의존성은 `raspberrypi-kernel-headers`, `libasound2-dev`,
`libmosquitto-dev`, `nlohmann-json3-dev` 입니다.

```
make                      # .ko 2개 + .dtbo 2개 + 유저 앱

sudo ./scripts/load.sh    # 오버레이 + 모듈 적재
sudo ./scripts/unload.sh  # 해제 (fbcon 언바인드 포함)

sudo ./app/alert-node     # 본체 실행 (Ctrl-C 로 종료)
```

아래 각 절은 개별로 다룰 때 참고합니다.


알림 노드 본체

`app/alert-node.conf` 를 실행 파일과 같은 디렉터리에서 읽습니다. `-c 경로` 로
다른 파일을 지정할 수 있습니다. 설정이 없으면 기본값으로 뜨고 그 사실을
출력합니다.

```
broker_host = localhost        # 배치할 때 서버 IP 로
node_id     = alarm_rpi_01     # AlarmCommand.target_device 와 비교
topic       = veda/alarm/control
audio_dir   = sounds           # 서버가 파일명만 보낼 때 찾는 곳
idle_text   = 감시 중          # 평상시 표시 (64px 를 넘으면 잘림)
```

`AlarmCommand` 의 각 필드가 이렇게 대응됩니다.

```
type           FALL / EGRESS → 빨강, VITAL_ABNORMAL → 주황, 그 외 초록
room           있으면 노드가 문구를 조립, 비면 message 를 그대로 표시
audio_action   PLAY / STOP        audio_file  파일명 또는 절대 경로
volume, loop   음량과 반복 재생    matrix_action  SHOW / CLEAR
matrix_passes  스크롤 횟수 (1~10 으로 잘림)      brightness  0~255
```

스크롤 도중 새 명령이 오면 즉시 끊고 최신 경보로 넘어갑니다.

로그를 파일이나 파이프로 넘길 때는 `stdbuf -oL` 을 붙이세요. 안 붙이면
출력이 버퍼에 갇혀 실시간으로 안 보입니다.

```
sudo stdbuf -oL ./app/alert-node | tee /tmp/alert.log
```


pulseaudio 회피 (기기당 한 번)

데스크톱 세션이 뜬 파이에는 pulseaudio 가 사운드카드를 잡습니다. 모듈을
리로드하면 5~10초 동안 PCM 을 붙들고 있어서 그 사이 재생이
`Device or resource busy` 로 실패합니다. lightdm 과 사용자 세션이 각각
띄우므로 두 번 겹치면 10초까지 갑니다.

udev 규칙으로 이 카드만 제외합니다. HDMI 오디오는 그대로 둡니다.

```
# /etc/udev/rules.d/91-pulseaudio-ignore-wm8960.rules
SUBSYSTEM!="sound", GOTO="pa_end"
KERNEL!="card*", GOTO="pa_end"
ATTRS{id}=="WM8960CustomSou", ENV{PULSE_IGNORE}="1"
LABEL="pa_end"
```

```
sudo udevadm control --reload-rules
sudo ./scripts/unload.sh && sudo ./scripts/load.sh
```



wm8960

```
cd drivers/wm8960

make

sudo dtoverlay wm8960_custum_daboyjo.dtbo

sudo modprobe regmap-i2c

sudo insmod wm8960_custum_daboyjo.ko

cd ../../app/AudioPlayer

./veda_player <음원파일경로>
```


hub75

64x32 HUB75 LED 패널 드라이버입니다. 오버레이 자체는 런타임에도 올라가지만
`isolcpus` 가 커널 부팅 파라미터라 결국 부트 설정 + 재부팅이 필요합니다.

먼저 부트 설정을 넣습니다. 기기당 한 번만 하면 되고, 재부팅이 필요합니다.
(현재 개발용 Pi 에는 이미 적용돼 있습니다. 아래로 확인 후 되어 있으면
이 단계는 건너뛰고 바로 make reload 로 갑니다.)

```
grep -E "hub75|audio|spi=off|gpio=12" /boot/config.txt
grep -o "isolcpus=[^ ]*" /boot/cmdline.txt
```

안 되어 있으면 아래를 추가합니다.

```
# /boot/config.txt
dtoverlay=hub75
gpio=12=op,dh,pu        # 부팅~로드 전 OE 소등 (없으면 패널 발열)
dtparam=spi=off         # SPI0 가 GPIO7~11 데이터핀 선점 방지
dtparam=audio=off       # 아날로그 오디오가 같은 PWM0 사용 - 충돌 방지

# /boot/cmdline.txt (한 줄에 이어서)
isolcpus=3 irqaffinity=0,1,2    # CPU3 = 리프레시 전용
```

빌드하고 오버레이를 배포합니다. 오버레이는 dts 를 고쳤을 때만 다시 하면 됩니다.

```
sudo apt install raspberrypi-kernel-headers

cd drivers/hub75

make                              # hub75.ko 와 hub75.dtbo 둘 다 생성

sudo cp hub75.dtbo /boot/overlays/

sudo reboot                       # 부트 설정이나 오버레이를 바꿨을 때만
```

모듈을 올립니다.

```
cd drivers/hub75

make reload      # 빌드 + fbcon 언바인드 + rmmod + insmod 를 한 번에

make unload      # 내릴 때
```

`insmod` 를 직접 쓰면 fbcon 이 fb0 을 잡고 있어 `rmmod` 가 거부됩니다.
`make reload` 가 그 처리까지 해주니 이쪽을 쓰는 게 좋습니다.

응급 소등 (패널이 켜진 채로 멈췄을 때)

```
sudo raspi-gpio set 12 op dh pu
```


matrix

패널에 한글 경보를 스크롤하는 앱입니다. hub75 모듈이 올라와 있어야 합니다.

```
cd app/matrix

make

sudo ./alert-demo      # Ctrl-C 로 종료
```

픽셀과 vsync 를 모두 `/dev/fbN` 하나로 처리합니다 (`FBIO_WAITFORVSYNC`).
프레임은 오프스크린에 완성한 뒤 프레임 경계에서 한 번에 복사합니다. 단일
버퍼라 이 순서를 깨고 fb 에 직접 그리면 스크롤이 찢어집니다.

이벤트는 아직 `alert-demo.cpp` 가 난수로 만듭니다. 렌더링은 `AlertDisplay`
(`alert-display.hpp`) 로 분리되어 있어, MQTT 연동은 이벤트 소스만 갈아끼우면
됩니다.


