CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic
INCLUDES := -Isrc

.PHONY: all bench run_maint run_match clean

all: bench

bench: bench/bench.cpp src/engine.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) bench/bench.cpp -o bench_run

run_maint: bench
	./bench_run --mode maintenance --ops 5000000 --seed 12345

run_match: bench
	./bench_run --mode match --ops 5000000 --seed 12345 --cross 90

clean:
	rm -f bench_run