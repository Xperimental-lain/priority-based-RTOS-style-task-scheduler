CC      = gcc
CXX     = g++
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Iinclude
CXXFLAGS= -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS =

BUILD   = build

.PHONY: all clean run-c run-cpp

all: $(BUILD)/demo_c $(BUILD)/demo_cpp

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rtos.o: src/rtos.c include/rtos.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/demo_c: examples/demo_c.c $(BUILD)/rtos.o | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/demo_cpp: examples/demo_cpp.cpp $(BUILD)/rtos.o | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

run-c: $(BUILD)/demo_c
	./$(BUILD)/demo_c

run-cpp: $(BUILD)/demo_cpp
	./$(BUILD)/demo_cpp

clean:
	rm -rf $(BUILD)
