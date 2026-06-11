# Reproducing the OMNIS baseline

This document is the operational recipe for reproducing the categorical baseline
(`data/results/baseline_<date>.csv`) on a clean checkout.

## Prerequisites

- macOS or Linux
- C++17 compiler (clang ≥ 14 or gcc ≥ 9)
- CMake ≥ 3.16
- zlib (for OEIS snapshot ingestion)
- ~80 GB free disk if you fetch every category at full N (smaller subsets
  are configurable)
- ~24 h of compute at default `--budget 600` (one CPU; faster machines scale
  linearly)

## 1. Clone and build

```bash
git clone <omnis-repo-url>
cd omnis
mkdir build-cmake && cd build-cmake
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Confirm the four binaries are built:

```bash
ls -la omnis omnis_validate gen_workload oeis_loader
```

Run the test suite to verify the build is healthy:

```bash
ctest --output-on-failure -j1
```

All five suites must pass before proceeding.

## 2. Fetch and pin the OEIS snapshot

```bash
cd ..
tools/oeis_fetch.sh
tools/oeis_keyword_fetch.sh
```

Both scripts SHA-pin every artefact into `data/oeis/SOURCES.md`. The pinned
SHAs are part of the reproduction contract — if upstream OEIS has updated,
the verification step will fail and you must either re-pin (`--refresh`)
or check out a matching revision of OMNIS.

Verify the snapshot:

```bash
( cd data/oeis/snapshot && shasum -a 256 -c stripped.gz.sha256 names.gz.sha256 keywords.tsv.sha256 )
```

## 3. (Optional) Regenerate workloads from the snapshot

The committed workloads under `data/categories/` were emitted under
`SOURCE_DATE_EPOCH=1746576000` so anyone re-emitting gets byte-identical
files. The default flow uses the committed files directly; only run this
step if you suspect drift or want to re-pin.

```bash
tools/gen_all.sh                       # rewrite + verify pin
tools/gen_all.sh --verify-only         # check committed workloads vs pin
```

## 4. Run the baseline

```bash
tools/run_paper_baseline.sh
```

This iterates every workload file in `data/categories/`, invoking
`omnis_validate` with `--freeze-db --budget 600` per sequence, and
appends each result to `data/results/baseline_<date>.csv`. A run
manifest containing git SHA, OS/CPU/compiler descriptions, snapshot
SHA, and total runtime is written alongside.

Common variants:

```bash
# Smaller smoke run (8s × 14 = ~2 min)
tools/run_paper_baseline.sh --budget 8 --categories benchmark14

# Restrict to OEIS subset
tools/run_paper_baseline.sh --categories oeis_core,oeis_hard
```

## 5. Acceptance gates

The acceptance contract:

| Gate | Check |
|---|---|
| E1   | `eca256` rows: count of `discovered ≥ 240`. (Paper baseline 244/256.) |
| E2   | aggregate `compressed_only / discovered ≤ 0.01`. |
| E3   | `neg_controls` rows: 0 `discovered`. `bench_pib4`: `neither`. |
| E4   | A second sweep on a fresh checkout reproduces every `(sc, pred_sc)` exactly and every `mdl` within ±0.5 bits. |

A simple aggregator:

```bash
awk -F, '
NR==1 {next}
$1 ~ /^eca_/ {eca[$8]++}
$1 ~ /^neg_/ {neg[$8]++}
$1 == "bench_pib4" {pib=$8}
{tot[$8]++}
END {
  printf "ECA discovered: %d\n", eca["discovered"]+0
  printf "Negative controls discovered: %d\n", neg["discovered"]+0
  printf "PiB4 class: %s\n", pib
  printf "Total cells: discovered=%d compressed_only=%d not_compressed_predicted=%d neither=%d\n",
    tot["discovered"]+0, tot["compressed_only"]+0,
    tot["not_compressed_predicted"]+0, tot["neither"]+0
  ratio = (tot["discovered"]+0 > 0) ? tot["compressed_only"]/tot["discovered"] : 0
  printf "compressed_only/discovered: %.4f\n", ratio
}
' data/results/baseline_*.csv
```

## 6. What can drift legitimately

- `time_s` — wall-clock per sequence; varies with hardware load.
- `solver_desc` — the engine may discover any of several equivalent programs
  for one target; the `(sc, pred_sc, mdl)` triple is what the determinism
  contract pins.

## 7. What must NOT drift

- Workload-file SHAs (verified via `data/categories/CHECKSUMS.sha256`).
- The OEIS snapshot SHAs (`data/oeis/SOURCES.md`).
- The CSV header (the binary itself emits it via `--csv-header` — single
  source of truth for the schema).
- The Solomonoff cell of any sequence at the same budget on the same
  hardware (within MDL ± 0.5 bits).

If you observe drift in any of those, treat it as a reproducibility
incident: capture the run manifest and the diverging row, file an issue
against the OMNIS repository.
