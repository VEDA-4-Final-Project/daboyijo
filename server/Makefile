TARGET   := daboyijo-server
BUILDDIR := build

SRCS := $(wildcard src/*.cpp) $(wildcard video/*.cpp) $(wildcard core/*.cpp)
OBJS := $(SRCS:%.cpp=$(BUILDDIR)/%.o)

PKGS     := opencv4 libavformat libavcodec libavutil libswscale

CXX      ?= g++
CXXFLAGS += -std=c++17 -Wall -Wextra -O2 -Isrc -Ivideo -Icore -I..
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

.PHONY: all run clean
