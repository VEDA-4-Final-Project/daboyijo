TARGET   := daboyijo-server
BUILDDIR := build

SRCS := $(wildcard src/*.cpp) $(wildcard video/*.cpp) $(wildcard core/*.cpp)
OBJS := $(SRCS:%.cpp=$(BUILDDIR)/%.o)

CXX      ?= g++
CXXFLAGS += -std=c++17 -Wall -Wextra -O2 -Isrc -Ivideo -Icore
CXXFLAGS += $(shell pkg-config --cflags opencv4)
LDLIBS   := $(shell pkg-config --libs opencv4) -lpthread

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
