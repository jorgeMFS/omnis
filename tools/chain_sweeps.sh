#!/usr/bin/env bash
# chain_sweeps.sh - sequential launcher. Runs sweeps one at a time so
# the engine's worker pool gets the full CPU per candidate and the
# determinism contract isn't contaminated by concurrent sweeps.
#
# Each stage waits for the previous to exit cleanly (PID gone).
#
# Usage:
#   tools/chain_sweeps.sh                      # default 3-stage chain (after medium)
#   tools/chain_sweeps.sh --budget 600         # explicit budget
#   tools/chain_sweeps.sh --status             # show chain progress
#
# Defaults are arranged paper-priority first:
#   1. eca256              (headline number; ~3-6 h alone)
#   2. large               (oeis_base + oeis_core; 2-4 days)
#   3. totalistic_3state   (2187 candidates; 3-5 days)
#
# Each stage launches in background via the launcher (nohup+caffeinate).
# Chain runner itself is nohup'd so it survives across days.
# If a sweep is already running when the chain starts, the chain waits
# politely for it to finish before launching its first stage.

set -eu
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$REPO_ROOT/data/results"
mkdir -p "$RESULTS_DIR"

BUDGET=600
CHAIN_LOG="$RESULTS_DIR/chain_$(date -u +%Y%m%dT%H%M%SZ).log"

while [ $# -gt 0 ]; do
    case "$1" in
        --budget) BUDGET="$2"; shift 2 ;;
        --status)
            if ! ls "$RESULTS_DIR"/chain_*.log >/dev/null 2>&1; then
                echo "no chain logs found"
            else
                for log in "$RESULTS_DIR"/chain_*.log; do
                    echo "=== $(basename "$log") ==="
                    tail -30 "$log"
                    echo ""
                done
            fi
            "$REPO_ROOT/tools/launch_sweep.sh" --status
            exit 0 ;;
        -h|--help)
            grep '^#' "$0" | head -25 | sed 's/^# \?//'
            exit 0 ;;
        *) echo "chain_sweeps: unknown option '$1'" >&2; exit 2 ;;
    esac
done

# Generate a self-contained worker script that the chain process will run.
WORKER="$RESULTS_DIR/.chain_worker_$(date -u +%Y%m%dT%H%M%SZ).sh"
cat > "$WORKER" <<EOF
#!/usr/bin/env bash
set -u
REPO_ROOT='$REPO_ROOT'
RESULTS_DIR='$RESULTS_DIR'
BUDGET='$BUDGET'
LOG='$CHAIN_LOG'

echo "chain_sweeps: starting at \$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "\$LOG"
echo "  budget per candidate: \${BUDGET}s" >> "\$LOG"
echo "" >> "\$LOG"

run_stage() {
    local label="\$1"
    local cats="\$2"
    # Wait for any currently running sweep to finish first.
    while pgrep -f "run_paper_baseline" >/dev/null 2>&1; do
        sleep 60
    done
    echo "[\$(date -u +%H:%M:%S)] launching stage '\$label' (\$cats)" >> "\$LOG"
    "\$REPO_ROOT/tools/launch_sweep.sh" --label "\$label" --budget "\$BUDGET" --categories "\$cats" >> "\$LOG" 2>&1
    local pidfile=\$(ls -t "\$RESULTS_DIR"/sweep_\${label}_*.pid 2>/dev/null | head -1)
    if [ -z "\$pidfile" ]; then
        echo "[\$(date -u +%H:%M:%S)] ERROR: no pid file for stage '\$label'; aborting chain" >> "\$LOG"
        return 1
    fi
    local pid=\$(cat "\$pidfile")
    echo "[\$(date -u +%H:%M:%S)] stage '\$label' running as PID \$pid; waiting for completion" >> "\$LOG"
    while kill -0 "\$pid" 2>/dev/null; do
        sleep 60
    done
    echo "[\$(date -u +%H:%M:%S)] stage '\$label' completed (PID \$pid exited)" >> "\$LOG"
    local csv=\$(ls -t "\$RESULTS_DIR"/baseline_*.csv 2>/dev/null | head -1)
    if [ -f "\$csv" ]; then
        local n=\$(awk -F, 'NR>1' "\$csv" | wc -l | tr -d ' ')
        local d=\$(awk -F, 'NR>1 && \$10=="discovered" {c++} END{print c+0}' "\$csv")
        echo "[\$(date -u +%H:%M:%S)]   \$(basename "\$csv"): \$n rows, \$d discovered" >> "\$LOG"
    fi
    echo "" >> "\$LOG"
}

run_stage eca256              "eca256"              || exit 1
run_stage large               "oeis_base,oeis_core" || exit 1
run_stage totalistic_3state   "totalistic_3state"   || exit 1

echo "chain_sweeps: finished at \$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "\$LOG"
EOF
chmod +x "$WORKER"

# Launch the chain worker in nohup+caffeinate so it survives.
if command -v caffeinate >/dev/null 2>&1; then
    nohup caffeinate -i bash "$WORKER" > /dev/null 2>&1 &
else
    nohup bash "$WORKER" > /dev/null 2>&1 &
fi
CHAIN_PID=$!
disown $CHAIN_PID 2>/dev/null || true
echo "$CHAIN_PID" > "${CHAIN_LOG%.log}.pid"

echo "chain runner launched as PID $CHAIN_PID"
echo "  chain log:    $CHAIN_LOG"
echo "  chain pid:    ${CHAIN_LOG%.log}.pid"
echo "  worker:       $WORKER"
echo ""
echo "Stages (run sequentially after current sweep finishes):"
echo "  1. eca256              eca256                    (~3-6 h)"
echo "  2. large               oeis_base,oeis_core       (~2-4 days)"
echo "  3. totalistic_3state   totalistic_3state         (~3-5 days)"
echo ""
echo "monitor:  tail -f $CHAIN_LOG"
echo "status:   tools/chain_sweeps.sh --status"
echo "kill all: pkill -KILL -f chain_worker; tools/launch_sweep.sh --kill"
