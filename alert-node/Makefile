# alert-node 전체 빌드 - 커널 모듈 2개 + 유저 앱
#
#   make            # drivers/*.ko, *.dtbo, app/matrix/alert-demo 전부
#   make drivers    # 커널 모듈만
#   make apps       # 유저 앱만
#   make clean
#
# 빌드 후 모듈 적재는 scripts/load.sh 참고.

DRIVERS := drivers/wm8960 drivers/hub75
APPS    := app/matrix app/AudioPlayer

.PHONY: all drivers apps clean $(DRIVERS) $(APPS)

all: drivers apps

drivers: $(DRIVERS)

apps: $(APPS)

$(DRIVERS) $(APPS):
	$(MAKE) -C $@

clean:
	for d in $(DRIVERS) $(APPS); do $(MAKE) -C $$d clean; done
