# 패널 알림 데모 빌드
#
#   make            # alert-demo 생성
#   sudo ./alert-demo
#
# hub75 드라이버가 올라와 있어야 한다 (fix.id 가 "hub75" 인 /dev/fbN 을 찾는다).
# vsync 는 표준 FBIO_WAITFORVSYNC 로 받으므로 /dev/hub75 는 필요 없다.

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17
SRCS     := alert-demo.cpp alert-display.cpp
APP      := alert-demo

.PHONY: all clean

all: $(APP)

$(APP): $(SRCS) alert-display.hpp alert-event.hpp hub75-font16.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(APP)
