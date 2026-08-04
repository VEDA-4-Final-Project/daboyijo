# HUB75 드라이버 + 오버레이 빌드용 Kbuild Makefile
#
# 이 Pi(Bullseye, 커널 6.1.21)에서 헤더 설치 후 빌드
#   sudo apt install raspberrypi-kernel-headers
#   make            # hub75.ko 와 hub75.dtbo 둘 다 생성
#   make reload     # 올리기 (오버레이 적용된 뒤에)
#   make unload     # 내리기
#
# insmod/rmmod 를 직접 쓰지 말 것. fbcon 이 fb0 을 물고 있어 rmmod 가
# "in use" 로 거부한다 (아래 fbcon-unbind 주석 참고).
#
# 커널 버전 주의: platform_driver.remove 는 6.10 까지 int 반환
# 6.11+ 에서 void 로 바뀜 - 커널 올리면 hub75_remove 를 void 로 고칠 것

obj-m += hub75.o

KDIR ?= /lib/modules/$(shell uname -r)/build
# PWD 는 셸이 물려준 환경변수라 make -C 로 들어와도 안 바뀐다.
# 상위 Makefile 에서 부를 때 M= 이 엉뚱한 곳을 가리키므로 CURDIR 을 쓴다.
DTS  := hub75-overlay.dts
DTBO := hub75.dtbo

.PHONY: all module overlay clean reload unload fbcon-unbind

all: module overlay

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

overlay: $(DTBO)

$(DTBO): $(DTS)
	dtc -@ -I dts -O dtb -o $@ $<

# HDMI fbdev 가 없어 우리가 fb0 을 차지 -> 커널 콘솔(fbcon)이 자동 바인딩 ->
# 모듈 refcount 를 쥐고 있어 rmmod 가 "in use" 로 거부한다.
# 내리기 전에 반드시 언바인드해야 한다 (reload/unload 공통).
fbcon-unbind:
	@for c in /sys/class/vtconsole/vtcon*; do \
		grep -q "frame buffer" "$$c/name" 2>/dev/null && \
		{ echo "fbcon unbind: $$c"; echo 0 | sudo tee "$$c/bind" >/dev/null; }; \
	done; true

# 빌드 -> (있으면) 내리기 -> 다시 올리기 를 한 방에
reload: all fbcon-unbind
	@if lsmod | grep -q "^hub75"; then sudo rmmod hub75; fi
	# sys_* 심볼 의존성 - 나중에 빌트인(=y)이 되면 modprobe 가 "not found" 나므로 - 로 무시
	-sudo modprobe fb_sys_fops sysfillrect syscopyarea sysimgblt
	sudo insmod hub75.ko

unload: fbcon-unbind
	@if lsmod | grep -q "^hub75"; then sudo rmmod hub75; \
	 else echo "hub75: 로드되어 있지 않음"; fi

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f $(DTBO)
