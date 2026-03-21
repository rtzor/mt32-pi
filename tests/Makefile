#
# tests/Makefile
#
# Builds and runs host-side unit tests for mt32-pi.
# Uses doctest (header-only) -- no dependencies beyond g++.
#
# Usage:
#   make            # build
#   make run        # build + run
#   make clean      # clean artifacts
#

CXX       = g++
CC        = gcc
CXXFLAGS  = -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -O0
CXXFLAGS += -I. -Istubs -Idoctest -I../include -I../external/inih -DUNIT_TEST
CFLAGS    = -std=c99 -I../external/inih

# C++ source files -- add new test_*.cpp here
SRCS      = test_main.cpp \
            test_midirouter.cpp \
            ../src/midirouter.cpp \
            test_audiomixer.cpp \
            ../src/audiomixer.cpp \
            test_audioeffects.cpp \
            ../src/audioeffects.cpp \
            test_midiparser.cpp \
            ../src/midiparser.cpp \
            test_config_parse.cpp \
            test_midirecorder.cpp \
            test_fluidsequencer.cpp \
            ../src/fluidsequencer.cpp \
            ../src/midirecorder.cpp \
            test_playlist.cpp \
            ../src/playlist.cpp \
            test_osc.cpp \
            ../src/net/oscdaemon.cpp \
            stubs/fluidsynth_stub.cpp \
            stubs/fatfs_stub.cpp

# C source files (inih)
CFILES    = ../external/inih/ini.c

OBJS      = $(SRCS:.cpp=.o)
COBJS     = $(CFILES:.c=.o)
TARGET    = run_tests

.PHONY: all run clean

all: $(TARGET)
	@echo "Build OK.  Run:  ./$(TARGET)"

run: $(TARGET)
	./$(TARGET)

$(TARGET): $(OBJS) $(COBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(COBJS) $(TARGET)
