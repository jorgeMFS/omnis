#!/usr/bin/env bash
# run_paper_baseline.sh — full categorical reproduction sweep.
#
# Runs omnis_validate over every workload in data/categories/, writing one
# CSV row per candidate to data/results/baseline_<date>.csv. Emits a run
# manifest alongside the CSV with: git SHA, OS/CPU/compiler, snapshot SHAs,
# total runtime. Both files together constitute the acceptance
# artefact (see docs/REPRODUCING.md, acceptance gates).
#
# Reproducibility contract:
#   - freeze-db is enforced (omnis_validate's default; never write to library)
#   - per-sequence budget is uniform (default 600s)
#   - every classification uses the canonical Solomonoff cell (compresses
#     AND predicts) — see docs/SOLOMONOFF_VALIDATION.md
#   - manifest captures the environment so others can verify their reroll
#     reproduces the (sc, pred_sc, MDL ± 0.5) determinism contract
#
# Usage:
#   tools/run_paper_baseline.sh [--budget S] [--output-dir DIR] [--categories LIST]
#
# Options:
#   --budget S         per-sequence wall budget in seconds (default 600)
#   --output-dir DIR   where to write baseline_<date>.csv + manifest
#                      (default data/results/)
#   --categories LIST  comma-separated subset of category names; default = all
#                      committed files under data/categories/
#   -h, --help         this message
#
# Exit codes:
#   0 sweep complete, CSV + manifest written
#   1 prerequisite missing (binary, workload file, etc.)
#   2 usage error
#   3 sweep produced unexpected I/O state

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-cmake"
CAT_DIR="$REPO_ROOT/data/categories"
SNAPSHOT_DIR="$REPO_ROOT/data/oeis/snapshot"

BUDGET=600
OUTPUT_DIR="$REPO_ROOT/data/results"
CATEGORIES_FILTER=""

RESUME_CSV=""
while [ $# -gt 0 ]; do
    case "$1" in
        --budget)         BUDGET="$2"; shift 2 ;;
        --output-dir)     OUTPUT_DIR="$2"; shift 2 ;;
        --categories)     CATEGORIES_FILTER="$2"; shift 2 ;;
        --resume)         RESUME_CSV="$2"; shift 2 ;;   # resume into an existing CSV (skip rows already present)
        -h|--help)
            sed -n '2,/^# Exit codes:/p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *)  echo "run_paper_baseline: unknown option '$1'" >&2; exit 2 ;;
    esac
done

VALIDATE_BIN="$BUILD_DIR/omnis_validate"
if [ ! -x "$VALIDATE_BIN" ]; then
    echo "run_paper_baseline: $VALIDATE_BIN not built. Run cmake + make first." >&2
    exit 1
fi

if [ ! -d "$CAT_DIR" ]; then
    echo "run_paper_baseline: $CAT_DIR missing. Run tools/gen_all.sh first." >&2
    exit 1
fi

# Verify checksums first — refuses to run on drifted workloads.
if [ -f "$CAT_DIR/CHECKSUMS.sha256" ]; then
    ( cd "$CAT_DIR" && shasum -a 256 -c CHECKSUMS.sha256 >/dev/null 2>&1 ) || {
        echo "run_paper_baseline: data/categories/ workloads drifted from CHECKSUMS pin." >&2
        echo "  Re-run tools/gen_all.sh --refresh-checksums to update or restore." >&2
        exit 3
    }
fi

mkdir -p "$OUTPUT_DIR"

# Resume path: --resume PATH skips candidates whose id is already in PATH and
# appends new rows to PATH. Without --resume, a fresh CSV is created.
if [ -n "$RESUME_CSV" ]; then
    if [ ! -f "$RESUME_CSV" ]; then
        echo "run_paper_baseline: --resume target '$RESUME_CSV' does not exist." >&2
        exit 3
    fi
    CSV_PATH="$RESUME_CSV"
    # Derive DATE_TAG from the original filename so the manifest sibling resolves.
    DATE_TAG="$(basename "$RESUME_CSV" .csv | sed 's/^baseline_//')"
    MANIFEST_PATH="$OUTPUT_DIR/baseline_${DATE_TAG}.manifest.txt"
    DONE_IDS=$(mktemp)
    # Extract already-completed candidate ids (column 1) into a sorted lookup
    # file. grep -F -x will skip any candidate whose id is in here.
    awk -F, 'NR>1 {print $1}' "$CSV_PATH" | sort -u > "$DONE_IDS"
    echo "run_paper_baseline: --resume mode, $(wc -l < "$DONE_IDS" | tr -d ' ') candidates already done in $CSV_PATH"
else
    DATE_TAG="$(date -u +%Y%m%dT%H%M%SZ)"
    CSV_PATH="$OUTPUT_DIR/baseline_${DATE_TAG}.csv"
    MANIFEST_PATH="$OUTPUT_DIR/baseline_${DATE_TAG}.manifest.txt"
    DONE_IDS=""
    # Header (canonical, asked from the binary itself).
    "$VALIDATE_BIN" --csv-header > "$CSV_PATH"
fi

# Build category list. Either explicit subset, or every *.txt in CAT_DIR.
if [ -n "$CATEGORIES_FILTER" ]; then
    IFS=',' read -ra REQ <<< "$CATEGORIES_FILTER"
    CATEGORIES=()
    for c in "${REQ[@]}"; do
        f="$CAT_DIR/${c}.txt"
        if [ ! -f "$f" ]; then
            echo "run_paper_baseline: category '$c' missing ($f)." >&2; exit 1
        fi
        CATEGORIES+=("$f")
    done
else
    CATEGORIES=()
    for f in "$CAT_DIR"/*.txt; do
        [ -f "$f" ] && CATEGORIES+=("$f")
    done
fi

START_T=$(date +%s)
TOTAL_LINES=0
for f in "${CATEGORIES[@]}"; do
    n=$(grep -cv -E '^(#|$)' "$f")
    TOTAL_LINES=$((TOTAL_LINES + n))
done
echo "run_paper_baseline: ${#CATEGORIES[@]} categories, $TOTAL_LINES candidates, ${BUDGET}s budget each."
echo "                    output: $CSV_PATH"

# Manifest fields:
# "All categorical runs use --freeze-db against an empty starting
#  program_db.bin. Library state is never an implicit input."
# Synthesize an empty db file per run (4-byte magic 'ENAR' + ver=2 + cnt=0).
# Engine ProgramDB::load accepts this and starts with zero entries; --freeze-db
# guarantees no writes during the sweep.
EMPTY_DB="$OUTPUT_DIR/empty_db_${DATE_TAG}.bin"
printf 'ENAR\x02\x00\x00\x00\x00\x00\x00\x00' > "$EMPTY_DB"

# ===== Manifest CHECKPOINT (guards against overnight subshell death) =====
# Write the static/known-at-start fields immediately. If the sweep is killed
# mid-loop (mac sleep, terminal close, OOM), the manifest still exists and
# points at the CSV; row-count from CSV reveals actual completion.
# A second write at the END appends `completion_status: complete` +
# `elapsed_seconds` + the cell tally. `sync` flushes to disk on each write.
# All probes wrapped with `|| true` inside the subshell so `set -e` doesn't
# kill us on non-zero exit (git in non-git dir, missing sysctl on Linux,
# missing files, etc.). Then the [ -z ... ] && fallback gives us "unknown".
GIT_SHA="$(git -C "$REPO_ROOT" rev-parse --verify HEAD 2>/dev/null || true)"
[ -z "$GIT_SHA" ] && GIT_SHA="unknown"
HOST_NAME="$(hostname 2>/dev/null || true)"
[ -z "$HOST_NAME" ] && HOST_NAME="unknown"
OS_DESC="$(uname -srm 2>/dev/null || true)"
[ -z "$OS_DESC" ] && OS_DESC="unknown"
CPU_DESC="$(sysctl -n machdep.cpu.brand_string 2>/dev/null \
            || (grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^[^:]*: //') \
            || true)"
[ -z "$CPU_DESC" ] && CPU_DESC="unknown"
COMPILER_DESC="$( (cc --version 2>/dev/null || gcc --version 2>/dev/null) | head -1 || true)"
[ -z "$COMPILER_DESC" ] && COMPILER_DESC="unknown"
SNAPSHOT_STRIPPED_SHA="$( (test -f "$SNAPSHOT_DIR/stripped.gz" \
                          && shasum -a 256 "$SNAPSHOT_DIR/stripped.gz" | cut -d' ' -f1) \
                          2>/dev/null || true)"
[ -z "$SNAPSHOT_STRIPPED_SHA" ] && SNAPSHOT_STRIPPED_SHA="not_present"
SNAPSHOT_NAMES_SHA="$( (test -f "$SNAPSHOT_DIR/names.gz" \
                          && shasum -a 256 "$SNAPSHOT_DIR/names.gz" | cut -d' ' -f1) \
                          2>/dev/null || true)"
[ -z "$SNAPSHOT_NAMES_SHA" ] && SNAPSHOT_NAMES_SHA="not_present"

{
    echo "# omnis baseline run manifest (schema + provenance contract)"
    echo "# CHECKPOINT WRITE — fields below are valid; completion_status updates at end."
    echo "run_id:                 baseline_${DATE_TAG}"
    echo "csv_path:               $CSV_PATH"
    echo "csv_schema_columns:     15  # id,category,oeis_xref,A,total_n,train_n,k,sc,pred_sc,solomonoff_class,mdl,raw_bits,ratio,time_s,solver_desc"
    echo "omnis_sha:              $GIT_SHA"
    echo "oeis_stripped_sha256:   $SNAPSHOT_STRIPPED_SHA"
    echo "oeis_names_sha256:      $SNAPSHOT_NAMES_SHA"
    echo "host:                   $HOST_NAME"
    echo "os:                     $OS_DESC"
    echo "cpu:                    $CPU_DESC"
    echo "compiler:               $COMPILER_DESC"
    echo "budget_seconds:         $BUDGET"
    echo "freeze_db:              true"
    echo "empty_db:               true  # §3: library state never an implicit input"
    echo "empty_db_path:          $EMPTY_DB"
    echo "categories_count:       ${#CATEGORIES[@]}"
    echo "candidates_count:       $TOTAL_LINES"
    echo "started_utc:            $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "completion_status:      in_progress"
    echo "categories:"
    for f in "${CATEGORIES[@]}"; do
        cat_name="$(basename "$f" .txt)"
        n=$(grep -cv -E '^(#|$)' "$f")
        echo "  - name: $cat_name"
        echo "    count: $n"
    done
} > "$MANIFEST_PATH"
sync "$MANIFEST_PATH" 2>/dev/null || true

DONE=0
SKIPPED=0
for f in "${CATEGORIES[@]}"; do
    cat_name="$(basename "$f" .txt)"
    while IFS= read -r line; do
        case "$line" in ''|\#*) continue ;; esac
        id_field="$(printf '%s' "$line" | awk '{print $1}')"
        # Resume: skip candidates whose id is already in the resume CSV.
        if [ -n "$DONE_IDS" ] && grep -qxF "$id_field" "$DONE_IDS" 2>/dev/null; then
            SKIPPED=$((SKIPPED + 1))
            continue
        fi
        # Per-row provenance columns.
        case "$id_field" in
            A[0-9]*) oeis_xref="$(printf '%s' "$id_field" | sed -E 's/_a[0-9]+$//')" ;;
            *)       oeis_xref="" ;;
        esac
        # 1.2× wall safety on top of solver budget; suspect a hang otherwise.
        timeout "$(awk "BEGIN{print int($BUDGET*1.2)+10}")" \
            "$VALIDATE_BIN" --budget "$BUDGET" --db "$EMPTY_DB" --freeze-db \
            --category "$cat_name" --oeis-xref "$oeis_xref" --out "$CSV_PATH" \
            <<< "$line" >/dev/null 2>&1 || true
        DONE=$((DONE + 1))
        if [ $((DONE % 25)) -eq 0 ]; then
            elapsed=$(($(date +%s) - START_T))
            echo "  progress: $DONE/$TOTAL_LINES new ($SKIPPED skipped, $cat_name) elapsed=${elapsed}s"
        fi
    done < "$f"
done
[ -n "$DONE_IDS" ] && rm -f "$DONE_IDS"

ELAPSED=$(($(date +%s) - START_T))

# ===== Manifest FINALIZE (appends completion lines to the checkpoint above) =====
# Sweep finished normally → rewrite the in_progress fields with final values.
# If this block never runs (signal, sleep, crash), the checkpoint manifest
# from the head of the script still exists; `completion_status: in_progress`
# tells the reader to consult CSV row count for actual progress.
ROWS_COMPLETED=$(awk -F, 'NR>1' "$CSV_PATH" | wc -l | tr -d ' ')

# Tally Solomonoff classifications. Column 10 is solomonoff_class in the
# 15-column schema (id, category, oeis_xref, A, total_n, train_n, k, sc,
# pred_sc, solomonoff_class, mdl, raw_bits, ratio, time_s, solver_desc).
DISCOVERED=$(awk -F, 'NR>1 && $10=="discovered"             {n++} END{print n+0}' "$CSV_PATH")
COMPONLY=$(awk -F,   'NR>1 && $10=="compressed_only"        {n++} END{print n+0}' "$CSV_PATH")
PREDONLY=$(awk -F,   'NR>1 && $10=="not_compressed_predicted"{n++} END{print n+0}' "$CSV_PATH")
NEITHER=$(awk -F,    'NR>1 && $10=="neither"                {n++} END{print n+0}' "$CSV_PATH")

# Atomic rewrite: write to .new then mv. Avoids torn-write if killed during.
{
    grep -v '^completion_status:\|^elapsed_seconds:\|^ended_utc:\|^rows_completed:\|^cells_' "$MANIFEST_PATH"
    echo "completion_status:      complete"
    echo "elapsed_seconds:        $ELAPSED"
    echo "ended_utc:              $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "rows_completed:         $ROWS_COMPLETED"
    echo "cells_discovered:               $DISCOVERED"
    echo "cells_compressed_only:          $COMPONLY"
    echo "cells_not_compressed_predicted: $PREDONLY"
    echo "cells_neither:                  $NEITHER"
} > "${MANIFEST_PATH}.new"
mv -f "${MANIFEST_PATH}.new" "$MANIFEST_PATH"
sync "$MANIFEST_PATH" 2>/dev/null || true

# ===== Summary =====
echo ""
echo "run_paper_baseline: COMPLETE in ${ELAPSED}s"
echo "  csv:      $CSV_PATH"
echo "  manifest: $MANIFEST_PATH"
echo "  Solomonoff cell counts:"
echo "    discovered:               $DISCOVERED"
echo "    compressed_only:          $COMPONLY"
echo "    not_compressed_predicted: $PREDONLY"
echo "    neither:                  $NEITHER"
