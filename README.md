# order-matching-engine

## Profiling (Codespaces)

Use the built-in perf workflow targets to profile safely in Codespaces without privileged kernel changes.

### Why this workflow works in Codespaces

- **`--` separator is required** after `perf record` so app flags like `--mode` and `--ops` are passed to `./exchange` instead of being misread as perf flags.
- **`-e cpu-clock` is used by default** because Codespaces often restrict hardware counters; `cpu-clock` is a software event that is usually available.
- Warnings about kernel symbols (`kptr_restrict`, `kallsyms`) are expected in hosted environments and are generally safe to ignore for user-space hotspot profiling.

### Commands

- Validate environment and fallback logic:
  - `make perf-check`
- Fast iteration profile (~200k ops):
  - `make perf-quick`
- Full profile (default 5,000,000 ops):
  - `make perf`

### Overrides

You can override workload parameters with make variables:

```bash
make perf PERF_MODE=maintenance PERF_OPS=1000000 PERF_SEED=42
make perf-quick PERF_MODE=match PERF_SEED=7
```

### Outputs

After `make perf` or `make perf-quick`, artifacts are generated in repo root:

- `perf.data`
- `perf_report.txt` (`perf report --stdio` output)
- `perf.script` (`perf script` output; flamegraph-ready for later processing)
