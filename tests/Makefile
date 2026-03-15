#
# tests/Makefile
#
# Builds and runs host-side unit tests for mt32-pi.
# Uses doctest (header-only) — no dependencies beyond g++.
#
# Usage:
#   make            # build
#   make run        # build + run
#   make clean      # clean artifacts
#

CXX       = g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -O0
CXXFLAGS += -I. -Istubs -Idoctest -I../include -DUNIT_TEST

# Source files — add new test_*.cpp here
SRCS      = test_main.cpp \
            test_midirouter.cpp \
            ../src/midirouter.cpp \
            test_audiomixer.cpp \
            ../src/audiomixer.cpp

OBJS      = $(SRCS:.cpp=.o)
TARGET    = run_tests

.PHONY: all run clean

all: $(TARGET)
	@echo "Build OK.  Run:  ./$(TARGET)"

run: $(TARGET)
	./$(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
