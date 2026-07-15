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


