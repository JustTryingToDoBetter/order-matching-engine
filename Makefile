CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic
INCLUDES := -Isrc

.PHONY: all bench clean

all: bench

bench: bench/bench.cpp src/engine.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) bench/bench.cpp -o bench_run

clean:
	rm -f bench_run