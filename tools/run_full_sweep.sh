#!/usr/bin/env bash
# run_full_sweep.sh — launches the Phase E paper-baseline sweep in the
# background via nohup so it survives terminal disconnection.
#
# Stages (sequential, each waits for previous to finish):
#   1. Small (already runnable in foreground): arithmetic, prime, selfref,
#      morphic, benchmark14, neg_controls
#   2. Medium: collatz_grid, eca256, oeis_morphic, oeis_cellular,
#      oeis_hard, oeis_base, oeis_core
#   3. Large: totalistic_3state (separate stage because it's >30% of total
#      compute at 2187 candidates)
#
# Each stage appends to data/results/baseline_<date>.csv. The manifest is
# written when that stage completes. To kill the sweep:
#   pgrep -f run_paper_baseline.sh ; kill -TERM <pid>
#
# Usage:
#   tools/run_full_sweep.sh [--budget S] [--skip-small]
#
# Default budget: 600s/seq (paper-spec). Override with --budget for a
# quick-pass sweep (e.g. --budget 60 finishes ~10× faster but only
# captures easy targets).

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUDGET=600
SKIP_SMALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --budget)     BUDGET="$2"; shift 2 ;;
        --skip-small) SKIP_SMALL=1; shift ;;
        -h|--help)
            sed -n '2,/^# Usage:/p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "run_full_sweep: unknown option '$1'" >&2; exit 2 ;;
    esac
done

LOG_DIR="$REPO_ROOT/data/results"
mkdir -p "$LOG_DIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$LOG_DIR/sweep_${STAMP}.log"

echo "run_full_sweep: writing log to $LOG"
echo "run_full_sweep: budget=${BUDGET}s; skip_small=$SKIP_SMALL"

(
    echo "=== sweep started $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    if [ "$SKIP_SMALL" = "0" ]; then
        echo "=== stage 1: small categories ==="
        "$REPO_ROOT/tools/run_paper_baseline.sh" --budget "$BUDGET" \
            --categories arithmetic,prime,selfref,morphic,benchmark14,neg_controls \
            --output-dir "$LOG_DIR"
    fi
    echo "=== stage 2: medium/large categories ==="
    "$REPO_ROOT/tools/run_paper_baseline.sh" --budget "$BUDGET" \
        --categories collatz_grid,eca256,oeis_morphic,oeis_cellular,oeis_hard,oeis_base,oeis_core \
        --output-dir "$LOG_DIR"
    echo "=== stage 3: totalistic_3state (largest single category) ==="
    "$REPO_ROOT/tools/run_paper_baseline.sh" --budget "$BUDGET" \
        --categories totalistic_3state \
        --output-dir "$LOG_DIR"
    echo "=== sweep complete $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
) > "$LOG" 2>&1 &

PID=$!
echo "run_full_sweep: launched PID $PID (nohup-equivalent via disowned subshell)"
echo "run_full_sweep: monitor with: tail -f $LOG"
echo "run_full_sweep: kill with:    kill -TERM $PID"
disown $PID 2>/dev/null || true
echo "$PID" > "$LOG_DIR/sweep_${STAMP}.pid"
