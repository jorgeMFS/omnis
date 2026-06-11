#!/usr/bin/env bash
# gen_all.sh — Regenerate every committed workload file from scratch.
#
# Calls:
#   - gen_workload  for the 9 local categories
#   - oeis_loader   for the 5 OEIS-sourced categories
#
# All emissions run under a fixed SOURCE_DATE_EPOCH so the workload-file
# headers (#created_utc) are deterministic across reruns. Body content is
# already deterministic via the body_sha256 contract; SOURCE_DATE_EPOCH
# additionally pins the whole-file SHA so data/categories/CHECKSUMS.sha256
# verifies on any host at any time.
#
# Prerequisites:
#   - gen_workload and oeis_loader built (cmake + make)
#   - data/oeis/snapshot/ populated (run tools/oeis_fetch.sh and
#     tools/oeis_keyword_fetch.sh first)
#
# Usage:
#   tools/gen_all.sh                         # rewrite all workloads + verify pin
#   tools/gen_all.sh --refresh-checksums     # also rewrite CHECKSUMS.sha256
#   tools/gen_all.sh --verify-only           # check committed files vs CHECKSUMS;
#                                              do NOT regenerate (use to detect
#                                              tampering / accidental edits)
#
# Exit codes:
#   0  all workloads written / verified
#   1  prerequisite missing (binary, snapshot, etc.)
#   2  gen_workload, oeis_loader, or checksum verification failed

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CAT_DIR="$REPO_ROOT/data/categories"
SNAPSHOT_DIR="$REPO_ROOT/data/oeis/snapshot"
BUILD_DIR="$REPO_ROOT/build-cmake"
CHECKSUMS="$CAT_DIR/CHECKSUMS.sha256"

REFRESH_CHECKSUMS=0
VERIFY_ONLY=0
case "${1:-}" in
    --refresh-checksums) REFRESH_CHECKSUMS=1 ;;
    --verify-only)       VERIFY_ONLY=1 ;;
    "")                  ;;
    *)                   echo "gen_all: unknown option '$1'" >&2; exit 1 ;;
esac

# --verify-only path is fast: just shasum -c, no rewrites, no binary needs.
if [ "$VERIFY_ONLY" = "1" ]; then
    if [ ! -f "$CAT_DIR/CHECKSUMS.sha256" ]; then
        echo "gen_all: $CAT_DIR/CHECKSUMS.sha256 missing — nothing to verify" >&2
        exit 1
    fi
    if ( cd "$CAT_DIR" && shasum -a 256 -c CHECKSUMS.sha256 >/dev/null 2>&1 ); then
        echo "gen_all: --verify-only OK ($(wc -l < "$CAT_DIR/CHECKSUMS.sha256" | tr -d ' ') files)"
        exit 0
    else
        echo "gen_all: --verify-only FAILED — workload file(s) drifted from pin:" >&2
        ( cd "$CAT_DIR" && shasum -a 256 -c CHECKSUMS.sha256 2>&1 | grep -v ': OK' )
        exit 2
    fi
fi

# Frozen SOURCE_DATE_EPOCH for the whole regen pass. 2026-05-07T00:00:00Z =
# epoch 1746576000. Anyone running this script anywhere gets the same header
# timestamp, so file SHAs are stable.
export SOURCE_DATE_EPOCH=1746576000

# Locate binaries.
GEN_WL="$BUILD_DIR/gen_workload"
OEIS_LD="$BUILD_DIR/oeis_loader"
for bin in "$GEN_WL" "$OEIS_LD"; do
    if [ ! -x "$bin" ]; then
        echo "gen_all: missing binary '$bin' — run cmake + make first" >&2
        exit 1
    fi
done
for f in stripped.gz names.gz keywords.tsv; do
    if [ ! -f "$SNAPSHOT_DIR/$f" ]; then
        echo "gen_all: missing snapshot file '$SNAPSHOT_DIR/$f' — run tools/oeis_fetch.sh and tools/oeis_keyword_fetch.sh first" >&2
        exit 1
    fi
done

mkdir -p "$CAT_DIR"

# Snapshot SHA = body SHA of stripped.gz (the bulk source of OEIS terms).
SNAPSHOT_SHA="$(shasum -a 256 "$SNAPSHOT_DIR/stripped.gz" | cut -d' ' -f1)"

emit() {
    local kind="$1"; shift
    local out="$CAT_DIR/$1.txt"
    echo "gen_all: -> $out"
    case "$kind" in
        local)
            "$GEN_WL" --category "$1" --out "$out" \
                || { echo "gen_all: gen_workload failed for $1" >&2; exit 2; }
            ;;
        oeis)
            "$OEIS_LD" --filter "$1" --snapshot-dir "$SNAPSHOT_DIR" \
                --snapshot-sha "$SNAPSHOT_SHA" --out "$out" \
                || { echo "gen_all: oeis_loader failed for $1" >&2; exit 2; }
            ;;
    esac
}

# Local categories (9). Order matches docs/CATEGORIES.md and MANIFEST.yaml.
emit local eca256
emit local totalistic_3state
emit local collatz_grid
emit local arithmetic
emit local selfref
emit local prime
emit local morphic
emit local neg_controls
emit local benchmark14

# OEIS-sourced categories (5).
emit oeis  oeis_core
emit oeis  oeis_hard
emit oeis  oeis_base
emit oeis  oeis_morphic
emit oeis  oeis_cellular

# Recompute CHECKSUMS.sha256 (when explicitly requested, or always — small
# enough that the side-effect of overwriting is harmless).
if [ "$REFRESH_CHECKSUMS" = "1" ] || [ ! -f "$CHECKSUMS" ]; then
    (
        cd "$CAT_DIR"
        # Sort the file list so CHECKSUMS.sha256 is itself reproducible.
        for f in $(ls *.txt | sort); do
            shasum -a 256 "$f"
        done
    ) > "$CHECKSUMS"
    echo "gen_all: wrote $CHECKSUMS"
else
    # Verify against existing pin.
    if ( cd "$CAT_DIR" && shasum -a 256 -c "$CHECKSUMS" >/dev/null 2>&1 ); then
        echo "gen_all: CHECKSUMS.sha256 verified"
    else
        echo "gen_all: CHECKSUMS.sha256 MISMATCH — re-run with --refresh-checksums if intentional" >&2
        exit 2
    fi
fi

echo "gen_all: done. $(ls -1 "$CAT_DIR"/*.txt | wc -l | tr -d ' ') workload files in $CAT_DIR"
