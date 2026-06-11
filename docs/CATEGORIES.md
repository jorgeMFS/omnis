# OMNIS workload categories

This is a human-readable summary of the 14 frozen workload files under
`data/categories/`. The structured truth (with body SHAs and selection
rules in machine-readable form) is `data/categories/MANIFEST.yaml`.

## At a glance

| Category | Count | Source | Alphabets | Notes |
|---|---:|---|---|---|
| `eca256` | 256 | local | A=2 | All 256 elementary CA rules. Paper baseline: 244/256 discovered. |
| `totalistic_3state` | 2187 | local | A=3 | All totalistic 3-state 3-neighbour CAs. |
| `collatz_grid` | 60 | local | mixed | (k,c,base) ∈ {3,5,7,9,11}×{1,3,5,7}×{2,3,4}; trajectory step counts. |
| `arithmetic` | 7 | local | A=4 (Liouville at A=2) | Number-theoretic functions: τ, φ, σ, Ω, ω, μ, λ. |
| `selfref` | 3 | local | A=2,4 | Kolakoski, Recamán, bit-reversal. |
| `prime` | 3 | local | A=2,4 | Prime indicator, prime gaps mod 4, primes mod 4. |
| `morphic` | 3 | local | A=2 | Rudin–Shapiro, Baum–Sweet, period-doubling. |
| `neg_controls` | 20 | local | A=2,4 | Pi-base-4, seeded random (10), seeded crypto-style (9). Negative controls. |
| `benchmark14` | 14 | local | mixed | 12 hand-curated reference targets + DivisorCount (A000005) + Sigma (A000203). |
| `oeis_core` | 780 | OEIS snapshot | A∈{2,3,4,5,7} | Sloane's curated core (`keyword:core ∧ keyword:nonn`). |
| `oeis_base` | 576 | OEIS snapshot | A∈{2,3,4} | `keyword:base ∧ keyword:nonn`. |
| `oeis_hard` | 267 | OEIS snapshot | A∈{2,3,4} | `keyword:hard ∧ keyword:nonn`. |
| `oeis_cellular` | 382 | OEIS snapshot | A∈{2,3} | Names containing "cellular automaton" / "wolfram" / "rule ". |
| `oeis_morphic` | 30 | OEIS snapshot | A∈{2,3} | Names containing "morphic". |

Total: **4,588 candidates** across all categories. The paper's four
analysed populations draw on a subset of these: S1 (collatz_grid,
oeis_morphic, oeis_cellular, oeis_hard; 739 raw), S2 (eca256; 256),
S3 (oeis_base, oeis_core; 1,356 raw), and S4 (totalistic_3state;
2,187), totalling 4,538 raw candidates before the length filter and
de-duplication described in the paper's SI. The remaining 50
candidates (arithmetic, selfref, prime, morphic, neg_controls,
benchmark14) are smoke tests and negative controls, not part of the
analysed corpus.

## Pre-registered acceptance targets

Recorded in `MANIFEST.yaml` under `expected_solve_rate_min` /
`expected_solve_rate_max`:

- `eca256`: ≥ 93 % `discovered` (target ≈ 244/256).
- `benchmark14`: ≥ 78 % `discovered` (target 11/14; pi-b4 + nested-loops out-of-budget under uniform settings).
- `neg_controls`: ≤ 5 % `discovered` (negative controls must mostly reject).
- `oeis_core`: ≥ 55 % `discovered`.

Other categories are exploratory and have no pre-registered floor.

## Reproducing a category

Three independent paths must agree (the reproducibility contract):

1. **Use the committed file as-is**:
   ```bash
   tools/run_paper_baseline.sh --categories oeis_core
   ```
2. **Regenerate from the OEIS snapshot**:
   ```bash
   tools/gen_all.sh --refresh-checksums    # rewrites + re-pins
   tools/gen_all.sh --verify-only          # checks committed vs current
   ```
3. **Verify the SHA pin**:
   ```bash
   ( cd data/categories && shasum -a 256 -c CHECKSUMS.sha256 )
   ```

## Editorial notes

- `keywords.tsv` in `data/oeis/snapshot/` is itself derived (search
  `keyword:{core,hard,base}`, paginated, `%K` extracted) - NOT a bulk
  dump. `oeis_loader.cpp` joins on this file when filtering.
- The `oeis_cellular` and `oeis_morphic` categories use *name* substring
  matching (since `keyword:cellular` is not a standard OEIS keyword).
  These selections are reproducible via `tools/gen_all.sh` against the
  pinned `stripped.gz` + `names.gz` SHAs.
- Categorical alphabet reduction (`t' = t mod A`) is performed at load
  time per category. The reduced-alphabet sequence is what the engine
  sees; the original OEIS index is preserved in the workload file as
  `oeis_xref:Annnnnn` for traceability.
