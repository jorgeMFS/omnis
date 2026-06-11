#!/bin/bash
# PGO build for OMNIS: profile-guided optimization
# Trains on 6 representative sequences covering all major code paths:
#   Counting (Phase 1 instant), TriMod8 (Phase 2A flat), ThueMorse (DARY),
#   Collatz (Phase 2B cascade), DivisorCount (Phase 2F WSBP NESTED_LOOP),
#   Rule30 (Phase 2C streaming wide-bit)
set -e

echo "=== PGO Step 1: Instrumented build ==="
g++ -std=c++17 -O3 -march=native -flto -fprofile-generate -I../src ../src/cli.cpp -o omnis_pgo_gen

echo "=== PGO Step 2: Training ==="
for f in ../data/categories/benchmark14.txt; do
    echo "  Training on workload: $f"
    while IFS= read -r line; do
        case "$line" in ''|\#*) continue ;; esac
        echo "$line" | ./omnis_pgo_gen - --budget 5 >/dev/null 2>&1 || true
    done < "$f"
done

echo "=== PGO Step 3: Merge profiles ==="
PROFDATA=$(xcrun --find llvm-profdata)
"$PROFDATA" merge -output=default.profdata *.profraw

echo "=== PGO Step 4: Optimized build ==="
g++ -std=c++17 -O3 -march=native -flto -fprofile-use=default.profdata -I../src ../src/cli.cpp -o omnis

echo "=== Cleanup ==="
rm -f omnis_pgo_gen *.profraw default.profdata

echo "PGO build complete: ./omnis"
