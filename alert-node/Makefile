# alert-node 전체 빌드 - 커널 모듈 2개 + 유저 앱
#
#   make            # drivers/*.ko, *.dtbo, app/alert-node(+matrix/AudioPlayer 데모) 전부
#   make drivers    # 커널 모듈만
#   make apps       # 유저 앱만
#   make clean
#
# 빌드 후 모듈 적재는 scripts/load.sh 참고.

DRIVERS := drivers/wm8960 drivers/hub75
# app/Makefile 이 알아서 matrix·AudioPlayer 데모까지 내려가므로 app 하나만
# 지정한다. 예전엔 app/matrix app/AudioPlayer 만 있어서 본체(app/alert-node)가
# 이 목록에서 아예 빠져 있었다 — 여기서 make 해도 알림 노드 본체는 안 바뀌던 원인.
APPS    := app

.PHONY: all drivers apps clean $(DRIVERS) $(APPS)

all: drivers apps

drivers: $(DRIVERS)

apps: $(APPS)

$(DRIVERS) $(APPS):
	$(MAKE) -C $@

clean:
	for d in $(DRIVERS) $(APPS); do $(MAKE) -C $$d clean; done
