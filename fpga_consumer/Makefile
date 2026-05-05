# ============================================================================
# Makefile for FPGA Tick Stream Consumer
# Uses Fifo5 lock-free SPSC ring buffer
# ============================================================================

CXX      = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
TARGET   = fpga_consumer

SRCS = fpga_tick_consumer.cpp
OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(TARGET) $(OBJS)

# Usage: make run PORT=/dev/cu.usbserial-XXXX
run: $(TARGET)
	./$(TARGET) $(PORT)
