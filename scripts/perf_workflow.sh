#!/usr/bin/env bash
set -euo pipefail

EXCHANGE_BIN="./exchange"
PERF_DATA="perf.data"
PERF_REPORT="perf_report.txt"
PERF_SCRIPT="perf.script"

usage() {
  cat <<'USAGE'
Usage:
  scripts/perf_workflow.sh check
  scripts/perf_workflow.sh run [--mode MODE] [--ops N] [--seed N]
  scripts/perf_workflow.sh quick [--mode MODE] [--seed N]

Environment overrides:
  PERF_MODE (default: maintenance)
  PERF_OPS  (default: 5000000, quick: 200000)
  PERF_SEED (default: 12345)
USAGE
}

require_exchange() {
  if [[ ! -x "$EXCHANGE_BIN" ]]; then
    echo "ERROR: $EXCHANGE_BIN not found or not executable. Build it first (make exchange or make perf-build)." >&2
    exit 1
  fi
}

run_exchange_sanity() {
  echo "Running exchange sanity check..."
  "$EXCHANGE_BIN" --mode maintenance --ops 1000 >/dev/null
}

check_perf_available() {
  if ! command -v perf >/dev/null 2>&1; then
    echo "ERROR: perf is not installed or not in PATH." >&2
    exit 1
  fi
  perf --version
}

cleanup_perf_artifacts() {
  rm -f perf.data perf.data.old
}

record_with_event() {
  local event="$1"
  local mode="$2"
  local ops="$3"
  local seed="$4"

  perf record -e "$event" -F 99 -g --call-graph dwarf -- \
    "$EXCHANGE_BIN" --mode "$mode" --ops "$ops" --seed "$seed"
}

write_reports() {
  if [[ ! -s "$PERF_DATA" ]]; then
    echo "ERROR: perf produced missing/empty $PERF_DATA; recording likely failed." >&2
    exit 1
  fi

  perf report --stdio -i "$PERF_DATA" > "$PERF_REPORT"
  perf script -i "$PERF_DATA" > "$PERF_SCRIPT"

  echo "Saved report: $PERF_REPORT"
  echo "Saved script: $PERF_SCRIPT"
}

run_check() {
  check_perf_available
  require_exchange
  run_exchange_sanity

  cleanup_perf_artifacts
  echo "Checking perf record using default task-clock event..."
  if perf record -e task-clock -F 99 -- "$EXCHANGE_BIN" --mode maintenance --ops 1000 >/dev/null 2>&1; then
    echo "perf check passed with task-clock."
    cleanup_perf_artifacts
    return 0
  fi

  echo "task-clock check failed; retrying with Codespaces-friendly software event cpu-clock..."
  cleanup_perf_artifacts
  if perf record -e cpu-clock -F 99 -- "$EXCHANGE_BIN" --mode maintenance --ops 1000 >/dev/null 2>&1; then
    echo "perf check passed with cpu-clock."
    cleanup_perf_artifacts
    return 0
  fi

  echo "ERROR: perf record failed with both task-clock and cpu-clock." >&2
  echo "Next steps: run 'make perf' for full diagnostics or inspect 'perf stat -- ./exchange --mode maintenance --ops 1000'." >&2
  exit 1
}

run_profile() {
  local mode="${PERF_MODE:-maintenance}"
  local ops="${PERF_OPS:-5000000}"
  local seed="${PERF_SEED:-12345}"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --mode) mode="$2"; shift 2 ;;
      --ops) ops="$2"; shift 2 ;;
      --seed) seed="$2"; shift 2 ;;
      *) echo "ERROR: unknown option: $1" >&2; usage; exit 1 ;;
    esac
  done

  check_perf_available
  require_exchange
  cleanup_perf_artifacts

  echo "Recording with cpu-clock (Codespaces-friendly) ..."
  record_with_event cpu-clock "$mode" "$ops" "$seed"
  write_reports
}

run_quick() {
  PERF_OPS="${PERF_OPS:-200000}" run_profile "$@"
}

cmd="${1:-}"
[[ -n "$cmd" ]] || { usage; exit 1; }
shift || true

case "$cmd" in
  check) run_check "$@" ;;
  run) run_profile "$@" ;;
  quick) run_quick "$@" ;;
  *) usage; exit 1 ;;
esac
