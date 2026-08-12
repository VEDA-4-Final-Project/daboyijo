#!/bin/sh
# alert-node 모듈 적재 - wm8960(오디오 코덱) + hub75(LED 패널)
#
#   sudo ./scripts/load.sh
#
# hub75 오버레이는 config.txt 의 dtoverlay=hub75 로 부팅 때 올라와 있어야 한다
# (README 의 부트 설정 참고). wm8960 오버레이만 여기서 런타임으로 올린다.

set -e
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "root 로 실행할 것 (sudo)"; exit 1; }

# --- wm8960 ---
dtoverlay -l | grep -q wm8960_custum_daboyjo ||
	dtoverlay -d drivers/wm8960 wm8960_custum_daboyjo

modprobe regmap-i2c			# 드라이버가 devm_regmap_init_i2c 사용
lsmod | grep -q '^wm8960_custum_daboyjo' ||
	insmod drivers/wm8960/wm8960_custum_daboyjo.ko

# --- hub75 ---
# fbdev 그리기 심볼이 별도 모듈(=m) 이라 insmod 로는 의존성이 안 풀린다
modprobe fb_sys_fops sysfillrect syscopyarea sysimgblt 2>/dev/null || true
lsmod | grep -q '^hub75' || insmod drivers/hub75/hub75.ko

echo "적재 완료"
lsmod | grep -E '^hub75|^wm8960_custum_daboyjo'
