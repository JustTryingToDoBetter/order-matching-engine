# order-matching-engine

MVP order-matching engine and benchmark harness focused on deterministic throughput measurement and profiling.

## Build and run

```bash
# Build benchmark driver
make bench

# Build exchange binary used by perf workflow
make exchange

# Run benchmark directly
./bench_run --mode maintenance --ops 5000000 --seed 12345
./bench_run --mode match --ops 5000000 --seed 12345 --cross 90
```

## Benchmark modes and key flags

`bench_run` supports two workload modes:

- `--mode maintenance`: mostly resting-book maintenance with configurable add/cancel/replace mix.
- `--mode match`: same operation mix, but add/replace prices are biased to cross the spread more often.

Key flags:

- `--ops <N>`: total operations (default `5000000`).
- `--seed <N>`: deterministic RNG seed (default `12345`).
- `--cross <0-100>`: crossing bias percentage in `match` mode (default `80`).
- `--add <0-100>`: add percentage (default `60`).
- `--cancel <0-100>`: cancel percentage (default `25`).
- `--replace <0-100>`: replace percentage (default `15`).

`--add + --cancel + --replace` must equal `100`.

Examples:

```bash
# Match-heavy workload with more crossing
./bench_run --mode match --ops 1000000 --seed 7 --cross 90

# Maintenance workload with custom operation mix
./bench_run --mode maintenance --ops 1000000 --seed 42 --add 50 --cancel 30 --replace 20
```

## Profiling in Codespaces

Use the built-in perf workflow targets to profile in Codespaces without privileged kernel changes.

Why this setup is Codespaces-friendly:

- Uses software event `cpu-clock` by default, which is usually available when hardware PMU events are restricted.
- Uses the required `--` separator after `perf record` so app flags (`--mode`, `--ops`, `--seed`) are passed to `./exchange`, not parsed as perf flags.

Commands:

```bash
make perf-check   # validate perf availability + event fallback logic
make perf-quick   # quick profile (200,000 ops)
make perf         # standard profile (5,000,000 ops)
```

Overrides:

```bash
make perf PERF_MODE=maintenance PERF_OPS=1000000 PERF_SEED=42
make perf-quick PERF_MODE=match PERF_SEED=7
```

Artifacts generated in repo root:

- `perf.data`
- `perf_report.txt` (`perf report --stdio` output)
- `perf.script` (`perf script` output; flamegraph-ready)

## Architecture overview (MVP)

The MVP engine (`OrderBookPool`) is optimized around predictable memory access and low allocator overhead:

- **Array price band**: fixed tick range (`900..1100`) mapped into contiguous bid/ask level arrays for O(1) level lookup.
- **Intrusive FIFO nodes**: each price level is a doubly-linked intrusive queue (`head/tail`) preserving time priority.
- **Pool allocator**: a freelist-backed `NodePool` recycles order nodes to minimize allocation churn.
- **Vector ID index**: `std::vector<OrderRef>` maps dense order IDs directly to node references for O(1) cancel/replace lookup.

## Benchmark results (before/after MVP)

Measured locally with seed `12345`, `--mode maintenance`, comparing:

- **Before**: array-level baseline (`engine_array.hpp` implementation)
- **After**: pool + intrusive implementation (`engine_pool.hpp`)

| Run size | Before ops/sec | After ops/sec | Delta |
|---|---:|---:|---:|
| Quick (200k ops) | 6.27M | 11.11M | +77.3% |
| Standard (5M ops) | 3.60M | 7.03M | +95.3% |

Headline changes:

- ~1.77x higher throughput on quick runs.
- ~1.95x higher throughput on standard runs.
- Improvement is primarily from removing per-order container churn and replacing hash/index indirection with dense vector indexing.

## Known constraints

- **Bounded tick range**: valid prices are restricted to `900..1100`; out-of-range orders are rejected.
- **Single-threaded engine**: matching, add, cancel, and replace are all executed on one thread.
- **Dense sequential order IDs in benchmark**: harness assumes incrementing IDs and pre-sizes ID-index structures accordingly.
