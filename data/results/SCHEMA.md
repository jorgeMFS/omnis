# `data/results/baseline_*.csv` schema

Normative specification of the categorical-sweep output. Every row of every
`baseline_<date>.csv` file emitted by `tools/run_paper_baseline.sh` (and
every standalone `omnis_validate --out PATH` invocation) conforms to this
schema.

This is the **frozen schema for v1**. Schema changes require bumping the
schema version in `data/categories/MANIFEST.yaml` and updating this
document.

## Columns (15)

| # | Name | Type | Description |
|--:|------|------|-------------|
| 1  | `id`                  | string  | Candidate id from the workload-file source line (column 1 of that line). For OEIS rows, of the form `Annnnnn_aA` where `A` is the reduced alphabet. For local rows, the generator's tag (e.g. `eca_030`, `bench_counting`). CSV-escaped if it contains a comma or quote. |
| 2  | `category`            | string  | Workload-family the candidate belongs to: one of `arithmetic, benchmark14, collatz_grid, eca256, morphic, neg_controls, oeis_base, oeis_cellular, oeis_core, oeis_hard, oeis_morphic, prime, selfref, totalistic_3state`. Set by `--category` from `tools/run_paper_baseline.sh`. Default `self` for direct CLI runs (not produced by the sweep runner). |
| 3  | `oeis_xref`           | string  | OEIS A-number for OEIS-sourced rows (e.g. `A000005`), empty string for local generators. Derived by `run_paper_baseline.sh` by stripping the `_aN` alphabet suffix from `id` when `id` matches `A\d+_a\d+`. |
| 4  | `A`                   | int     | Alphabet size of the candidate. Output symbols are in `[0, A)`. |
| 5  | `total_n`             | int     | Total length of the candidate (= training + held-out). |
| 6  | `train_n`             | int     | Training-prefix length presented to `solve()`. By construction `train_n = total_n - k`. |
| 7  | `k`                   | int     | Held-out length used for prediction. By construction `k = max(20, total_n / 4)` (locked in `INTEGRATION_PLAN.md` §9 item 2). |
| 8  | `sc`                  | int     | Training-match score: number of positions `i ∈ [0, train_n)` where the discovered program's output at `i` equals `t[i]`. `sc == train_n` is exact training match. |
| 9  | `pred_sc`             | int     | Held-out-match score: number of positions `k' ∈ [0, k)` where the discovered program's `train_n + k'`-th output equals `t[train_n + k']`. `pred_sc == k` is exact prediction. |
| 10 | `solomonoff_class`    | string  | One of: `discovered`, `compressed_only`, `not_compressed_predicted`, `neither`. Defined by the contingency: `compresses = (sc == train_n) AND (mdl < train_n × log2(A))`; `predicts = (pred_sc == k)`. See `docs/SOLOMONOFF_VALIDATION.md` for the formal spec. |
| 11 | `mdl`                 | float (2 dp) | Description length in bits of the discovered program under the canonical encoding (`computeMDL` in `src/omnis.cpp`). 0 if `sc == 0` (no candidate found). |
| 12 | `raw_bits`            | float (2 dp) | Uniform-prior raw bit-cost of the candidate: `total_n × log2(max(2, A))`. Reference baseline. |
| 13 | `ratio`               | float (4 dp) | `mdl / raw_bits` over the total candidate (NOT just training). Lower is more compressive; `discovered` rows typically have `ratio < 1.0`. |
| 14 | `time_s`              | float (3 dp) | Wall-clock time spent in `solve()` for this candidate (seconds). Not part of the determinism contract (see §3). |
| 15 | `solver_desc`         | string  | Short description of the discovered program (e.g. `NESTED_LOOP L=5`, `CTX_X nr=2 d=2 perm=(0,1) out=R0 L=1`, `FLAT_ISA_PREV`). Advisory; not part of the determinism contract. CSV-escaped. |

## CSV-escaping rules

A field is double-quoted iff it contains a comma, double-quote, or newline.
Inside a quoted field, embedded double-quotes are doubled (`"` → `""`).
The runner's `csv_escape` lambda implements this.

## Per-row provenance vs sweep-level manifest

The plan-locked schema in `INTEGRATION_PLAN.md` §5 originally included
nine sweep-level provenance fields per row: `omnis_version, omnis_sha,
generator_sha, oeis_snapshot_sha, run_id, run_date_utc, host, budget_s,
freeze_db`. These are constants across all rows of a single sweep (they
identify the sweep, not the candidate), so replicating them on every row
wastes storage and de-normalises the schema. Pass-8 PhD++ alignment keeps
those constants in the sibling `<csv>.manifest.txt` file written by
`tools/run_paper_baseline.sh`; this file is normative and must be
distributed alongside the CSV.

The two fields that ARE per-row (because they vary across rows of the same
sweep) — `category` and `oeis_xref` — were added to the CSV as columns 2
and 3.

## Manifest companion file

For every `baseline_<date>.csv`, `tools/run_paper_baseline.sh` writes a
sibling `baseline_<date>.manifest.txt` with the following keys (YAML-style
flat list):

```
run_id, csv_path, csv_schema_columns, omnis_sha, generator_sha,
oeis_stripped_sha256, oeis_names_sha256, host, os, cpu, compiler,
budget_seconds, freeze_db, empty_db, empty_db_path, categories_count,
candidates_count, elapsed_seconds, categories[]
```

`empty_db: true` confirms the §3 contract: the engine started this sweep
with zero library entries (no implicit Phase-0 testExtensions input). The
`empty_db_path` records where the synthesised empty `ENAR` magic file
lives so the reproducer can re-create it bit-for-bit.

## Determinism contract scope (§3)

Across two reruns sharing `(omnis_sha, generator_sha, oeis_snapshot_sha,
budget_seconds, freeze_db, empty_db)`, the contract is:

- `id, category, oeis_xref, A, total_n, train_n, k`: identical (input).
- `sc, pred_sc, solomonoff_class`: identical (modulo a single
  documented exception: thread-pool ordering on equal-`sc==train_n`
  candidates can swap which solver wins the lex tie-break; `mdl` may then
  shift by up to ±0.5 bits and the resulting `compresses` boundary may
  flip the class for a borderline row, which `tests/test_engine_determinism.cpp`
  bounds).
- `mdl`: stable within `±0.5` bits (see exception above).
- `raw_bits, ratio`: identical (derived from inputs + `mdl`).
- `time_s`: NOT part of the contract (hardware-dependent).
- `solver_desc`: advisory; may differ on ties.

## Aggregation conventions

`tools/aggregate_results.sh` groups rows by column 2 (`category`) and
counts `solomonoff_class` cells per group, producing a per-category
contingency table plus an aggregate `compressed_only / discovered` ratio
(Phase E2 acceptance target: `≤ 0.01`).
