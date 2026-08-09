TARGET   := daboyijo-server
BUILDDIR := build

SRCS := $(wildcard src/*.cpp) $(wildcard video/*.cpp) $(wildcard core/*.cpp) $(wildcard modules/*.cpp)
OBJS := $(SRCS:%.cpp=$(BUILDDIR)/%.o)
DEPS := $(OBJS:.o=.d)

PKGS     := opencv4 libavformat libavcodec libavutil libswscale libcurl libcrypto

CXX      ?= g++
# -MMD -MP: 헤더 의존성(.d) 자동 생성 — .hpp 수정 시 그걸 쓰는 .o 전부 재빌드
# (없으면 구조체 레이아웃이 바뀌어도 일부 .o만 재컴파일돼 링크 후 segfault)
CXXFLAGS += -std=c++17 -Wall -Wextra -O2 -MMD -MP -Isrc -Ivideo -Icore -Imodules -I..
# third_party: nlohmann/json 단일 헤더 (텔레그램 getUpdates·Gemini 응답 JSON 파싱)
CXXFLAGS += -Ithird_party
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lpthread

CXXFLAGS += $(shell mariadb_config --cflags)
LDLIBS   += $(shell mariadb_config --libs)
LDLIBS += -Wl,--allow-multiple-definition


# ── MoveNet(TFLite C++) 연동 ──────────────────────────────────
# 사전 준비 (라즈베리에서 1회, server/tools/movenet_bench.py로 속도 확인 후):
#   git clone --branch v2.14.0 --depth 1 https://github.com/tensorflow/tensorflow.git ~/tensorflow_src
#   mkdir -p ~/tflite_build && cd ~/tflite_build
#   cmake -DCMAKE_BUILD_TYPE=Release -DTFLITE_ENABLE_XNNPACK=ON ../tensorflow_src/tensorflow/lite
#   cmake --build . -j$(nproc)
# 아래 두 변수를 실제 빌드 결과 경로에 맞춰 조정할 것 (다르면 make 시 override:
#   make TFLITE_SRC=... TFLITE_BUILD=...):
TFLITE_SRC   ?= $(HOME)/tensorflow_src
TFLITE_BUILD ?= $(HOME)/tflite_build

CXXFLAGS += -I$(TFLITE_SRC)
# flatbuffers 헤더 위치는 TFLite 버전에 따라 달라질 수 있음 — 컴파일러가
# "flatbuffers/flatbuffers.h 없음" 이라고 하면:
#   find $(TFLITE_BUILD) -name flatbuffers.h
# 로 찾아서 이 줄의 경로를 그 디렉터리(…/include)로 바꿀 것.
CXXFLAGS += -I$(TFLITE_BUILD)/flatbuffers/include
# TFLite CMake 빌드는 libtensorflow-lite.a 외에 의존 라이브러리(XNNPACK,
# ruy, flatbuffers, pthreadpool, fft2d, farmhash, cpuinfo …)를 빌드 트리
# 곳곳에 별도 .a로 흩어 놓는다. 전부 모아 --start-group으로 묶어야
# 아카이브 간 상호 참조(undefined reference)가 풀린다.
TFLITE_LIBS := $(shell find $(TFLITE_BUILD) -name '*.a')
LDLIBS   += -Wl,--start-group $(TFLITE_LIBS) -Wl,--end-group -ldl


all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR)/$(TARGET): $(OBJS)
	$(CXX) $^ -o $@ $(LDLIBS)

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: all
	$(BUILDDIR)/$(TARGET) config/cameras.conf

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)

.PHONY: all run clean
