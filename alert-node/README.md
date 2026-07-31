# alert-node — 알림 노드 (Raspberry Pi 4)

담당: 김예훈, 이교민, 전승현

- MQTT 구독(`dbj/alert/+/cmd`) → 낙상 확정 시 오디오 사이렌 + LED 플래싱
- 오디오/LED 디바이스 드라이버 직접 구현 (프로젝트 필수 요구사항)
- 클라이언트發 해제 신호 처리



wm8960 

현재까지 사용법 작성해 두겠습니다. 

```
cd wm8960

make 

sudo dtoverlay  wm8960_custum_daboyjo.dtbo 

sudo modeprobe regmap-i2c 

sudo insmod  wm8960_custum_daboyjo.ko 

cd ..

cd AudioPlayer

./veda_player <음원파일경로> 
```


hub75

64x32 HUB75 LED 패널 드라이버입니다. wm8960 과 달리 런타임 dtoverlay 로는
안 올라가고, config.txt 설정 + 재부팅이 필요합니다.

먼저 부트 설정을 넣습니다. (한 번만, 재부팅 필요)

```
# /boot/config.txt
dtoverlay=hub75
gpio=12=op,dh,pu        # 부팅~로드 전 OE 소등 (없으면 패널 발열)
dtparam=spi=off         # SPI0 가 GPIO7~11 데이터핀 선점 방지
dtparam=audio=off       # 아날로그 오디오가 같은 PWM0 사용 - 충돌 방지

# /boot/cmdline.txt (한 줄에 이어서)
isolcpus=3 irqaffinity=0,1,2    # CPU3 = 리프레시 전용
```

빌드하고 오버레이를 배포합니다.

```
sudo apt install raspberrypi-kernel-headers

cd hub75

make                              # hub75.ko 와 hub75.dtbo 둘 다 생성

sudo cp hub75.dtbo /boot/overlays/

sudo reboot
```

재부팅 후 모듈을 올립니다.

```
cd hub75

make reload      # 빌드 + fbcon 언바인드 + rmmod + insmod 를 한 번에

make unload      # 내릴 때
```

`insmod` 를 직접 쓰면 fbcon 이 fb0 을 잡고 있어 `rmmod` 가 거부됩니다.
`make reload` 가 그 처리까지 해주니 이쪽을 쓰는 게 좋습니다.

응급 소등 (패널이 켜진 채로 멈췄을 때)

```
sudo raspi-gpio set 12 op dh pu
```


