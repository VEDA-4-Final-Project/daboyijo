# AudioPlayer 빌드
#
#   make        # veda_player, 2veda_player 둘 다
#
# veda_player   main.cpp  - WAV 경로를 받아 재생하는 기본 테스트
# 2veda_player  main2.cpp - 일시정지/재개/볼륨 제어 테스트
#
# 재생 장치는 VedaAudioPlayer 기본값(plughw:CARD=WM8960CustomSou)을 쓴다.
# 카드 번호는 부팅 설정에 따라 바뀌므로 번호로 열지 말 것.

CXX      := g++
CXXFLAGS := -O2 -Wall
LDLIBS   := -lasound -pthread

APPS := veda_player 2veda_player

.PHONY: all clean

all: $(APPS)

veda_player: main.cpp VedaAudioPlayer.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

2veda_player: main2.cpp VedaAudioPlayer.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(APPS)
