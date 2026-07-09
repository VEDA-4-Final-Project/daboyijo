TARGET   := daboyijo-server
BUILDDIR := build

SRCS := $(wildcard src/*.cpp) $(wildcard video/*.cpp) $(wildcard core/*.cpp)
OBJS := $(SRCS:%.cpp=$(BUILDDIR)/%.o)
DEPS := $(OBJS:.o=.d)

PKGS     := opencv4 libavformat libavcodec libavutil libswscale

CXX      ?= g++
# -MMD -MP: 헤더 의존성(.d) 자동 생성 — .hpp 수정 시 그걸 쓰는 .o 전부 재빌드
# (없으면 구조체 레이아웃이 바뀌어도 일부 .o만 재컴파일돼 링크 후 segfault)
CXXFLAGS += -std=c++17 -Wall -Wextra -O2 -MMD -MP -Isrc -Ivideo -Icore -I..
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lpthread

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
