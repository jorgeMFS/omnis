#!/usr/bin/env bash
# aggregate_results.sh - produces per-category and aggregate Solomonoff
# contingency counts from one or more baseline_*.csv files.
#
# Usage:
#   tools/aggregate_results.sh [csv_file_or_dir]
#
# If a directory is given, all *.csv files inside are aggregated.
# If a file is given, only that file is aggregated.
# If no argument is given, data/results/ is scanned.

set -u
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TGT="${1:-$REPO_ROOT/data/results}"

if [ -d "$TGT" ]; then
    INPUTS=$(ls "$TGT"/baseline_*.csv 2>/dev/null || true)
elif [ -f "$TGT" ]; then
    INPUTS="$TGT"
else
    echo "aggregate_results: '$TGT' is neither a file nor a directory" >&2
    exit 2
fi

if [ -z "$INPUTS" ]; then
    echo "aggregate_results: no baseline_*.csv found under $TGT" >&2
    exit 1
fi

cat $INPUTS | awk -F, '
NR==1 || $1=="id" {next}
{
    # 15-column schema: id, category, oeis_xref, A, total_n, train_n, k,
    # sc, pred_sc, solomonoff_class, mdl, raw_bits, ratio, time_s, solver_desc
    # category is column 2 (1-indexed) - explicit per-row provenance,
    # no longer derived from id.
    cat = $2
    cell = $10
    n[cat,cell]++; total[cat]++; agg[cell]++; total_all++
    if (cell == "discovered") disc_all++
    if (cell == "compressed_only") co_all++
}
END {
    printf "%-22s %12s %12s %12s %12s %12s %12s\n",
        "category","count","disc","disc_pct","comp_only","not_comp","neither"
    printf "%-22s %12s %12s %12s %12s %12s %12s\n",
        "--------","-----","----","--------","---------","--------","-------"
    # Emit category rows; sort externally for portability across awk variants.
    for (cat in total) {
        d = n[cat,"discovered"]+0
        co = n[cat,"compressed_only"]+0
        nc = n[cat,"not_compressed_predicted"]+0
        ne = n[cat,"neither"]+0
        t = total[cat]
        printf "ROW\t%-22s\t%d\t%d\t%.1f\t%d\t%d\t%d\n",
            cat, t, d, 100.0*d/t, co, nc, ne
    }
    printf "TOTAL\t%-22s\t%d\t%d\t%.1f\t%d\t%d\t%d\n",
        "TOTAL", total_all,
        agg["discovered"]+0, 100.0*(agg["discovered"]+0)/total_all,
        agg["compressed_only"]+0,
        agg["not_compressed_predicted"]+0, agg["neither"]+0
    if (disc_all > 0) {
        ratio = (co_all+0) / disc_all
        printf "RATIO\tcompressed_only / discovered = %.4f  (acceptance target: <= 0.01)\n", ratio
    }
}
' | (
    printf "%-22s %8s %8s %8s %12s %12s %8s\n" "category" "count" "disc" "disc_%" "comp_only" "not_comp" "neither"
    printf "%-22s %8s %8s %8s %12s %12s %8s\n" "--------" "-----" "----" "------" "---------" "--------" "-------"
    grep "^ROW" | sort -k2 | awk -F'\t' '{printf "%-22s %8s %8s %7s%% %12s %12s %8s\n", $2, $3, $4, $5, $6, $7, $8}'
    echo ""
    grep "^TOTAL" | awk -F'\t' '{printf "%-22s %8s %8s %7s%% %12s %12s %8s\n", $2, $3, $4, $5, $6, $7, $8}'
    grep "^RATIO" | sed 's/^RATIO\t//'
)
