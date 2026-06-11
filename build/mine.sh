#!/bin/bash
# OMNIS - multi-sequence shell wrapper.
#
# Default mode: runs `omnis` over each line of a workload file and writes a
# TSV row per result.
#
# --validate mode: runs `omnis_validate` instead, which splits each candidate
# into train + held-out (K = max(20, total_n/4)), classifies into the
# Solomonoff contingency, and emits one CSV data row per candidate plus a
# header at the top of the output.
#
# Usage:
#   mine.sh <budget_seconds> <workload_file> [output_path]
#   mine.sh --validate <budget_seconds> <workload_file> [output_path]
#
# Examples:
#   ./mine.sh 60 ../data/categories/benchmark14.txt
#   ./mine.sh --validate 60 ../data/categories/oeis_core.txt results_oeis_core.csv
#
# In --validate mode the engine writes diagnostic printf to stdout during
# solve(); we discard that and rely on omnis_validate's --out option to
# isolate the structured CSV row.
set -e
cd "$(dirname "$0")"

VALIDATE=0
if [ "${1:-}" = "--validate" ]; then
    VALIDATE=1
    shift
fi

BUDGET="${1:-60}"
WORKLOAD="${2:-../data/categories/benchmark14.txt}"

if [ "$VALIDATE" = "1" ]; then
    OUT="${3:-validated_$(date +%Y%m%d_%H%M%S).csv}"
    # Look in cwd (./) and the canonical CMake build directory.
    if   [ -x "./omnis_validate" ];                  then BIN="./omnis_validate"
    elif [ -x "../build-cmake/omnis_validate" ];     then BIN="../build-cmake/omnis_validate"
    else echo "FATAL: omnis_validate not built. Run 'cmake .. && make omnis_validate' first."; exit 1; fi
    [ -f "$WORKLOAD" ] || { echo "FATAL: workload not found: $WORKLOAD"; exit 1; }
    echo "Mining (validate) workload $WORKLOAD with ${BUDGET}s/target -> $OUT"

    # Header. The CSV header is the single source of truth for the schema, so
    # we ask the binary itself rather than hard-coding it here.
    "$BIN" --csv-header > "$OUT"

    while IFS= read -r line; do
        case "$line" in ''|\#*) continue ;; esac
        # Engine debug goes to /dev/null; CSV row is appended to $OUT via --out.
        echo "$line" | "$BIN" - --budget "$BUDGET" --freeze-db --out "$OUT" 2>/dev/null >/dev/null
    done < "$WORKLOAD"
else
    OUT="${3:-mining_$(date +%Y%m%d_%H%M%S).csv}"
    if   [ -x "./omnis" ];                  then BIN="./omnis"
    elif [ -x "../build-cmake/omnis" ];     then BIN="../build-cmake/omnis"
    else echo "FATAL: omnis not built. Run ./build.sh or 'cmake .. && make' first."; exit 1; fi
    [ -f "$WORKLOAD" ] || { echo "FATAL: workload not found: $WORKLOAD"; exit 1; }
    echo "Mining workload $WORKLOAD with ${BUDGET}s/target -> $OUT"
    printf "id\tsc/N\tpct\ttime_s\tMDL\traw_bits\tratio\tsolver\tstatus\n" > "$OUT"
    while IFS= read -r line; do
        case "$line" in ''|\#*) continue ;; esac
        echo "$line" | "$BIN" - --budget "$BUDGET" 2>/dev/null >> "$OUT"
    done < "$WORKLOAD"
fi

echo "Done. See $OUT"
