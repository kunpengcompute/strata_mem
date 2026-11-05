CXX ?= c++
AR ?= ar
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS ?=
BUILD ?= build

.PHONY: all clean
all: $(BUILD)/libl3_mem.a $(BUILD)/l3_mem_test

$(BUILD):
	mkdir -p $@

$(BUILD)/l3_mem.o: src/l3_mem.cpp src/l3_mem.h src/l3_mem_internal.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/libl3_mem.a: $(BUILD)/l3_mem.o
	$(AR) rcs $@ $<

$(BUILD)/l3_mem_test: test/l3_mem_test.cpp test/l3_mem_workload.h src/l3_mem.h $(BUILD)/libl3_mem.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libl3_mem.a $(LDFLAGS) -pthread -o $@

clean:
	rm -f build/l3_mem.o build/libl3_mem.a build/l3_mem_test
