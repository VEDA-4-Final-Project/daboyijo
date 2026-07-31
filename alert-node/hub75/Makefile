# HUB75 드라이버 + 오버레이 빌드용 Kbuild Makefile
#
# 이 Pi(Bullseye, 커널 6.1.21)에서 헤더 설치 후 빌드
#   sudo apt install raspberrypi-kernel-headers
#   make                     # hub75.ko 와 hub75.dtbo 둘 다 생성
#   sudo insmod hub75.ko     # 오버레이 적용된 뒤에
#   sudo rmmod  hub75
#
# 커널 버전 주의: platform_driver.remove 는 6.10 까지 int 반환
# 6.11+ 에서 void 로 바뀜 - 커널 올리면 hub75_remove 를 void 로 고칠 것

obj-m += hub75.o

KDIR ?= /lib/modules/$(shell uname -r)/build
DTS  := hub75-overlay.dts
DTBO := hub75.dtbo

.PHONY: all module overlay clean reload unload

all: module overlay

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

overlay: $(DTBO)

$(DTBO): $(DTS)
	dtc -@ -I dts -O dtb -o $@ $<

# 빌드 -> (있으면) 내리기 -> 다시 올리기 를 한 방에
reload: all
	-@for c in /sys/class/vtconsole/vtcon*; do \
		grep -q "frame buffer" "$$c/name" 2>/dev/null && \
		{ echo "fbcon unbind: $$c"; echo 0 | sudo tee "$$c/bind" >/dev/null; }; \
	done
	-sudo rmmod hub75
	# sys_* 심볼 의존성 - 나중에 빌트인(=y)이 되면 modprobe 가 "not found" 나므로 - 로 무시
	-sudo modprobe fb_sys_fops sysfillrect syscopyarea sysimgblt
	sudo insmod hub75.ko

unload:
	-sudo rmmod hub75

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(DTBO)
