# 알림 노드 본체 빌드 + 하위 데모 빌드
#
#   make            # alert-node 와 하위 데모(matrix, AudioPlayer) 전부
#   make node       # 본체만
#   sudo ./alert-node [-c 설정파일]
#
# 필요: libmosquitto-dev, nlohmann-json3-dev, libasound2-dev

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17
MQTT     := ../../MQTT/MQTT_prod
INCS     := -I$(MQTT) -IAudioPlayer -Imatrix
LDLIBS   := -lmosquitto -lasound -pthread

SRCS := main.cpp matrix/alert-display.cpp AudioPlayer/VedaAudioPlayer.cpp \
        $(MQTT)/MqttClient_veda.cpp
NODE := alert-node
SUBS := matrix AudioPlayer

.PHONY: all node clean $(SUBS)

all: node $(SUBS)

node: $(NODE)

$(NODE): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCS) -o $@ $(SRCS) $(LDLIBS)

$(SUBS):
	$(MAKE) -C $@

clean:
	rm -f $(NODE)
	for d in $(SUBS); do $(MAKE) -C $$d clean; done
