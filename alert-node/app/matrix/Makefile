# LED 매트릭스 알림 앱 빌드
#
#   make            # hub75-alert 생성
#   sudo ./hub75-alert
#
# hub75 드라이버가 올라와 있어야 한다 (fix.id == "hub75" 인 /dev/fbN 을 찾는다).
# vsync 는 표준 FBIO_WAITFORVSYNC 로 받으므로 /dev/hub75 는 필요 없다.

CC     := gcc
CFLAGS := -O2 -Wall
APPS   := hub75-alert

.PHONY: all clean

all: $(APPS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

# 한글 비트맵 헤더에 의존 (hub75-driver 의 genfont.py 로 생성된 파일)
hub75-alert: hub75-font16.h

clean:
	rm -f $(APPS)
