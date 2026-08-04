# alert-node — 알림 노드 (Raspberry Pi 4)

담당: 김예훈, 이교민, 전승현

- MQTT 구독(`dbj/alert/+/cmd`) → 낙상 확정 시 오디오 사이렌 + LED 플래싱
- 오디오/LED 디바이스 드라이버 직접 구현 (프로젝트 필수 요구사항)
- 클라이언트發 해제 신호 처리


구성

```
drivers/wm8960    오디오 코덱 드라이버 (커널 모듈 + 오버레이)
drivers/hub75     64x32 LED 패널 드라이버 (커널 모듈 + 오버레이)
app/AudioPlayer   ALSA 재생 (VedaAudioPlayer)
app/matrix        패널 알림 앱
scripts/          모듈 적재/해제
```

최상위에서 한 번에 빌드하고 올릴 수 있습니다. 아래 각 절은 개별로 다룰 때
참고합니다.

```
make                      # .ko 2개 + .dtbo 2개 + 유저 앱

sudo ./scripts/load.sh    # 오버레이 + 모듈 적재
sudo ./scripts/unload.sh  # 해제 (fbcon 언바인드 포함)
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

sudo ./hub75-alert      # Ctrl-C 로 종료
```

픽셀과 vsync 를 모두 `/dev/fbN` 하나로 처리합니다 (`FBIO_WAITFORVSYNC`).
프레임은 오프스크린에 완성한 뒤 프레임 경계에서 한 번에 복사합니다. 단일
버퍼라 이 순서를 깨고 fb 에 직접 그리면 스크롤이 찢어집니다.

이벤트는 아직 `gen_event()` 에서 난수로 만듭니다. MQTT 연동은 그 함수에
붙이면 됩니다.


