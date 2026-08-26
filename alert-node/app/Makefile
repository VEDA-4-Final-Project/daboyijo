# 알림 노드 본체 빌드 + 하위 데모 빌드
#
#   make            # alert-node 와 하위 데모(matrix, AudioPlayer) 전부
#   make node       # 본체만
#   sudo ./alert-node [-c 설정파일]
#
# 필요: libmosquitto-dev, nlohmann-json3-dev, libasound2-dev

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 $(shell pkg-config --cflags gstreamer-1.0)
MQTT     := ../../MQTT/MQTT_prod
INCS     := -I$(MQTT) -IAudioPlayer -Imatrix -Istreaming
LDLIBS   := -lmosquitto -lasound -pthread $(shell pkg-config --libs gstreamer-1.0)

SRCS := main.cpp matrix/alert-display.cpp AudioPlayer/VedaAudioPlayer.cpp $(MQTT)/MqttClient_veda.cpp streaming/AudioReceiver.cpp
    
# 헤더도 의존성에 넣는다 - 없으면 글꼴이나 표시기 헤더만 고쳤을 때 make 가
# "Nothing to be done" 하고 넘어가, 예전 바이너리를 새 것으로 착각하게 된다
HDRS := matrix/alert-display.hpp matrix/alert-event.hpp matrix/hub75-font16.h \
        AudioPlayer/VedaAudioPlayer.hpp
NODE := alert-node
SUBS := matrix AudioPlayer

.PHONY: all node clean $(SUBS)

all: node $(SUBS)

node: $(NODE)

$(NODE): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(INCS) -o $@ $(SRCS) $(LDLIBS)

$(SUBS):
	$(MAKE) -C $@

clean:
	rm -f $(NODE)
	for d in $(SUBS); do $(MAKE) -C $$d clean; done
