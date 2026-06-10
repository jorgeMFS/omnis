#!/usr/bin/env bash
# launch_sweep.sh — multi-day-safe wrapper around tools/run_paper_baseline.sh.
#
# Why this exists: the bare runner gets killed by macOS power management
# during idle sleep (overnight runs lose hours and the manifest doesn't
# finalise). This wrapper:
#   1. Runs under `caffeinate -i` so the OS treats the process as keeping
#      the system awake (idle-sleep prevented, display sleep allowed).
#   2. Wraps in `nohup` + disowned background so terminal close is harmless.
#   3. Writes a PID file so the sweep can be tracked + killed cleanly.
#   4. Routes stdout+stderr to a stable log file.
#
# Usage:
#   tools/launch_sweep.sh [--budget S] [--categories LIST] [--label TAG]
#   tools/launch_sweep.sh --status         # show running sweeps
#   tools/launch_sweep.sh --kill           # kill the most recent sweep
#
# Examples:
#   tools/launch_sweep.sh --label medium \
#       --categories collatz_grid,oeis_morphic,oeis_cellular,oeis_hard
#   tools/launch_sweep.sh --label full     # default = all categories
#   tools/launch_sweep.sh --status

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$REPO_ROOT/data/results"
mkdir -p "$LOG_DIR"

# Subcommands.
case "${1:-}" in
    --status)
        echo "Active sweeps:"
        for pidfile in "$LOG_DIR"/sweep_*.pid; do
            [ -f "$pidfile" ] || continue
            pid=$(cat "$pidfile" 2>/dev/null)
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                echo "  PID $pid  $(basename "$pidfile" .pid)"
                echo "    log: ${pidfile%.pid}.log"
                # Quick stats from CSV if present.
                base="${pidfile%.pid}"
                csv="${base/sweep_/baseline_}.csv"
                if [ -f "$csv" ]; then
                    n=$(awk -F, 'NR>1' "$csv" | wc -l | tr -d ' ')
                    echo "    rows so far: $n"
                fi
            else
                echo "  (stale) $(basename "$pidfile" .pid) — PID $pid not running"
            fi
        done
        exit 0 ;;
    --kill)
        for pidfile in "$LOG_DIR"/sweep_*.pid; do
            [ -f "$pidfile" ] || continue
            pid=$(cat "$pidfile" 2>/dev/null)
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                echo "killing sweep PID $pid ($(basename "$pidfile" .pid))"
                # Kill the whole process group: caffeinate + nohup + bash + omnis_validate.
                pkill -TERM -P "$pid" 2>/dev/null || true
                kill -TERM "$pid" 2>/dev/null || true
                sleep 1
                pkill -KILL -P "$pid" 2>/dev/null || true
                kill -KILL "$pid" 2>/dev/null || true
            fi
        done
        # Also kill any orphaned omnis_validate from a sweep.
        pkill -TERM -f "omnis_validate.*--out.*data/results" 2>/dev/null || true
        exit 0 ;;
esac

BUDGET=600
CATEGORIES=""
LABEL="sweep"
AUTO_RESUME=0

while [ $# -gt 0 ]; do
    case "$1" in
        --budget)     BUDGET="$2"; shift 2 ;;
        --categories) CATEGORIES="$2"; shift 2 ;;
        --label)      LABEL="$2"; shift 2 ;;
        --auto-resume) AUTO_RESUME=1; shift ;;
        -h|--help)
            sed -n '2,/^# Examples:/p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "launch_sweep: unknown option '$1'" >&2; exit 2 ;;
    esac
done

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TAG="${LABEL}_${STAMP}"
LOG="$LOG_DIR/sweep_${TAG}.log"
PIDFILE="$LOG_DIR/sweep_${TAG}.pid"

# Auto-resume: if --auto-resume was passed AND there is an existing in-progress
# CSV for this label, hand it to run_paper_baseline.sh via --resume. Otherwise
# the runner creates a fresh CSV. The heuristic for "in-progress for this label":
# (1) a manifest exists for label-named sweep with completion_status: in_progress
# (2) AND the sibling .csv file exists
# (3) AND no later sibling-CSV with completion_status: complete exists for the label
# Picks the most recent matching CSV. If anything is ambiguous, we fall back to
# a fresh CSV (safer than guessing wrong).
RESUME_ARGS=()
if [ "$AUTO_RESUME" = "1" ]; then
    candidate=""
    for mf in $(ls -t "$LOG_DIR"/baseline_*.manifest.txt 2>/dev/null); do
        # Manifests are tagged by run_id but not by label directly. The chain
        # writes both a sweep_${LABEL}_${STAMP}.log and a baseline_${STAMP}.csv,
        # with the timestamps matching to within ~1 second. We pair them via
        # the closest preceding sweep_${LABEL}_*.log file.
        stamp=$(basename "$mf" .manifest.txt | sed 's/^baseline_//')
        label_log=$(ls "$LOG_DIR"/sweep_${LABEL}_*.log 2>/dev/null | head -1)
        [ -z "$label_log" ] && continue
        # Check this manifest belongs to a run of *this* label by matching
        # within a 10-second window of the sweep_<LABEL>_*.log timestamp.
        log_stamp=$(basename "$label_log" .log | sed 's/^sweep_'"$LABEL"'_//')
        # Lexicographic comparison of YYYYMMDDTHHMMSSZ stamps is exact-time
        # ordering. Only consider manifests within the same minute as the log.
        [ "${stamp:0:13}" = "${log_stamp:0:13}" ] || continue
        status=$(grep -E '^completion_status:' "$mf" | awk '{print $2}')
        if [ "$status" = "in_progress" ]; then
            candidate="$LOG_DIR/baseline_${stamp}.csv"
            break  # newest in-progress one wins (ls -t is mtime-desc)
        fi
        if [ "$status" = "complete" ]; then
            # Most recent is already complete; nothing to resume.
            break
        fi
    done
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        echo "launch_sweep: --auto-resume found in-progress CSV → $candidate"
        RESUME_ARGS=(--resume "$candidate")
    else
        echo "launch_sweep: --auto-resume found nothing to resume; launching fresh"
    fi
fi

# Build the runner command.
CMD=("$REPO_ROOT/tools/run_paper_baseline.sh" --budget "$BUDGET" --output-dir "$LOG_DIR" "${RESUME_ARGS[@]}")
if [ -n "$CATEGORIES" ]; then
    CMD+=(--categories "$CATEGORIES")
fi

# `caffeinate -i` prevents idle sleep. -s would prevent system sleep entirely
# (heavier); -i is enough for nohup'd background work.
# Disowned subshell carries `caffeinate` as the PID we track — caffeinate
# inherits SIGTERM and forwards it to the runner child cleanly.
if command -v caffeinate >/dev/null 2>&1; then
    nohup caffeinate -i "${CMD[@]}" > "$LOG" 2>&1 &
    PID=$!
    echo "launched with caffeinate -i; PID $PID; sleep-safe"
else
    nohup "${CMD[@]}" > "$LOG" 2>&1 &
    PID=$!
    echo "launched bare (no caffeinate available); PID $PID; NOT sleep-safe"
fi
disown $PID 2>/dev/null || true
echo "$PID" > "$PIDFILE"

cat <<EOF

label:    $LABEL
log:      $LOG
pid file: $PIDFILE
csv:      $LOG_DIR/baseline_${STAMP}.csv  (created by runner)
manifest: $LOG_DIR/baseline_${STAMP}.manifest.txt  (checkpoint immediately, finalized at end)

monitor:  tail -f $LOG
status:   tools/launch_sweep.sh --status
kill:     tools/launch_sweep.sh --kill   (kills ALL active sweeps)
agg:      tools/aggregate_results.sh $LOG_DIR/

EOF
