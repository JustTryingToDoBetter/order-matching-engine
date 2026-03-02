CXX := g++
BASE_CXXFLAGS := -std=c++20 -Wall -Wextra -pedantic
CXXFLAGS := $(BASE_CXXFLAGS) -O2
PERF_CXXFLAGS := $(BASE_CXXFLAGS) -O3 -g -fno-omit-frame-pointer
INCLUDES := -Isrc

PERF_MODE ?= maintenance
PERF_OPS ?= 5000000
PERF_SEED ?= 12345

.PHONY: all bench exchange perf-build perf perf-quick perf-check run_maint run_match clean

all: bench exchange

bench: bench/bench.cpp src/engine.hpp src/engine_pool.hpp src/engine_array.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) bench/bench.cpp -o bench_run

exchange: bench/bench.cpp src/engine.hpp src/engine_pool.hpp src/engine_array.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) bench/bench.cpp -o exchange

perf-build: bench/bench.cpp src/engine.hpp src/engine_pool.hpp src/engine_array.hpp
	$(CXX) $(PERF_CXXFLAGS) $(INCLUDES) bench/bench.cpp -o exchange

# Profiling notes:
# - Use `--` after `perf record` so exchange flags (--mode/--ops/--seed) are never parsed as perf flags.
# - Use software event `cpu-clock` because Codespaces commonly restricts hardware counters.
perf: perf-build
	PERF_MODE=$(PERF_MODE) PERF_OPS=$(PERF_OPS) PERF_SEED=$(PERF_SEED) ./scripts/perf_workflow.sh run

perf-quick: perf-build
	PERF_MODE=$(PERF_MODE) PERF_OPS=200000 PERF_SEED=$(PERF_SEED) ./scripts/perf_workflow.sh quick

perf-check: exchange
	./scripts/perf_workflow.sh check

run_maint: bench
	./bench_run --mode maintenance --ops 5000000 --seed 12345

run_match: bench
	./bench_run --mode match --ops 5000000 --seed 12345 --cross 90

clean:
	rm -f bench_run exchange perf.data perf.data.old perf_report.txt perf.script
