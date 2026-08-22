#!/bin/sh
# alert-node 모듈 해제 - load.sh 의 역순
#
#   sudo ./scripts/unload.sh

set -e
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "root 로 실행할 것 (sudo)"; exit 1; }

# --- hub75 ---
# HDMI fbdev 가 없어 hub75 가 fb0 을 차지 -> 커널 콘솔(fbcon)이 자동 바인딩 ->
# 모듈 refcount 를 쥐고 있어 rmmod 가 "in use" 로 거부한다. 먼저 언바인드.
for c in /sys/class/vtconsole/vtcon*; do
	grep -q "frame buffer" "$c/name" 2>/dev/null &&
		{ echo "fbcon unbind: $c"; echo 0 > "$c/bind"; }
done; true

! lsmod | grep -q '^hub75' || rmmod hub75

# --- wm8960 ---
# 오버레이를 먼저 내려야 i2c 디바이스가 사라지면서 드라이버 바인딩이 풀린다.
# 순서를 바꾸면 refcount 가 1 이라 rmmod 가 "in use" 로 거부한다.
! dtoverlay -l | grep -q wm8960_custum_daboyjo ||
	dtoverlay -r wm8960_custum_daboyjo
! lsmod | grep -q '^wm8960_custum_daboyjo' || rmmod wm8960_custum_daboyjo

echo "해제 완료"
