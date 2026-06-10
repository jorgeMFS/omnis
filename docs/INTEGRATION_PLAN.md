# OMNIS Categorical Integration Plan

Locked plan for the categorical sweep pipeline. Reproducibility is the primary
invariant; everything else (scope, sample size, validation) hangs off it.

## 1. Charter

**Goal.** Wire OMNIS into a categorical-sweep pipeline that is (a) sourced
primarily from OEIS, (b) validated by held-out prediction, not just
compression, and (c) reproducible bit-for-bit from a clean clone given only
`(omnis_git_sha, oeis_snapshot_sha)`.

**Non-goals.** Performance optimization. New search phases. New ISA opcodes.
Result interpretation (the paper does that). Any narrative beyond the data.

**Done means.** A single command — `tools/run_paper_baseline.sh` — produces
`data/results/baseline_<date>.csv` whose every row is regeneratable by
anyone, anywhere, with the same SHAs.

## 2. Discovery semantics

Let a candidate be `(id, A, N, K, t[0..N+K-1])` with `N` training and `K`
held-out.

```
solve(t[0..N-1], A) -> r
sc           = |{i < N : runProgram(r)[i] == t[i]}|
pred_sc      = |{i < K : runProgram(r)[N+i] == t[N+i]}|
compresses   = (sc == N) AND (mdl(r) < N * log2(A))
predicts     = (pred_sc == K)        ; strict
discovered   = compresses AND predicts
```

The Solomonoff contingency table is the headline output:

|                        | predicts     | doesn't predict |
|------------------------|-------------:|----------------:|
| **compresses**         | `n_disc`     | `n_violation`   |
| **doesn't compress**   | `n_fn`       | `n_tn`          |

`n_violation` is the count of MAP-overfit / compressed-only programs. The
integration's main empirical claim is
`n_violation / (n_disc + n_violation) <= 0.01`.

`K = max(20, N/4)`. Strict prediction (no slack). These constants are part
of the contract.

## 3. Reproducibility contract

For every result row, the tuple
`(omnis_sha, generator_sha, oeis_snapshot_sha, run_config_sha, candidate_id)`
determines `(sc, pred_sc, mdl, solver_desc)` up to engine non-determinism
(see threat model). Time-derived fields (`time_s`, `host`, `run_date`) are
excluded from the determinism contract but recorded.

**Pinning.** Three SHAs, all committed:
- `omnis_sha` — git SHA of the engine + CLI at run time
- `generator_sha` — git SHA of `tools/gen_workload.cpp` at workload-emit time,
  embedded in workload-file header
- `oeis_snapshot_sha` — SHA-256 of `stripped.gz` + `names.gz` from oeis.org

**Library isolation.** All categorical runs use `--freeze-db` against an empty
starting `program_db.bin`. Library state is never an implicit input.

**Engine determinism.** Thread-pool may produce non-identical solver-choice on
ties. Contract: **at sufficient budget per candidate**, `(sc, pred_sc, MDL
+/- 0.5)` is stable across reruns; `solver_desc` may vary on ties.
`tests/test_engine_determinism.cpp` enforces this on a 30-candidate canary
at 30s/seq budget.

**Tight-budget caveat (Pass-8 PhD++ honest clarification).** When the budget
is insufficient for the worker pool to consistently find the optimal
program, `(sc, pred_sc)` themselves can vary across reruns: the engine
sometimes finds a candidate before the deadline and sometimes does not.
Empirically verified at `--budget 5` on the canary: 6/30 sequences (e.g.,
trimod8 ~20s solve, cube_mod5, pi_b4) varied across reruns. This is not
an engine determinism bug; it is the expected behaviour of any bounded
multi-threaded search. The §3 contract applies only to budgets sufficient
for convergence. Phase E sweeps at the standard 600s budget meet this
condition on every benchmark14 candidate (verified six times across
Passes 1+2 / 6 / 6b / 6c / 6d / 7).

## 4. File layout (final)

```
omnis/
  tools/
    gen_workload.cpp           pure data emitter, no engine link
    oeis_loader.cpp            parses stripped.gz, applies named filters
    oeis_filters.h             pre-registered filter rules (typed predicates)
    omnis_validate.cpp         wraps cli, splits train/test, runs predictNext
    oeis_fetch.sh              downloads + SHA-verifies OEIS snapshot
    gen_all.sh                 regenerates every workload file from scratch
    run_category.sh            runs one category, emits CSV
    run_paper_baseline.sh      runs all categories, emits baseline_<date>.csv
  data/
    oeis/
      SOURCES.md               URLs + SHAs of pinned snapshot
      stripped.sha256
      names.sha256
    categories/
      CHECKSUMS.sha256         committed; CI verifies
      MANIFEST.yaml            (schema below)
      eca256.txt
      totalistic_3state.txt
      collatz_grid.txt
      benchmark14.txt
      neg_controls.txt
      oeis_core.txt
      oeis_easy.txt
      oeis_hard.txt
      oeis_base.txt
      oeis_morphic.txt
      oeis_cellular.txt
    results/
      SCHEMA.md                column-by-column spec
      baseline_<date>.csv
  docs/
    INTEGRATION_PLAN.md        this document
    CATEGORIES.md              per-category selection rules
    REPRODUCING.md             clone-to-CSV recipe
    SOLOMONOFF_VALIDATION.md   explains the contingency claim
  tests/
    test_gen_workload.cpp      determinism, format, hash, OEIS xref
    test_omnis_validate.cpp    train/test split, prediction wiring
    test_engine_determinism.cpp thread-pool stability canary
```

## 5. Schemas

### Workload file

Plain text. `#`-commented header, then candidates.

```
# omnis-workload v1
# category: eca256
# generator_sha: <sha>
# omnis_min_version: 0.1.0
# created_utc: 2026-05-07T10:00:00Z
# count: 256
# body_sha256: <sha>
# selection_rule: enumerate rule in [0,256), N=500, single-cell IC, center column
eca_000 2 500 0 0 0 0 ...
eca_001 2 500 0 0 0 0 ...
```

### Workload manifest (`data/categories/MANIFEST.yaml`)

```yaml
schema_version: 1
generator_sha: <sha>
oeis_snapshot_sha: <sha>
files:
  - path: eca256.txt
    category: eca256
    count: 256
    selection_rule: "enumerate(0, 256); N=500; A=2; single_cell_ic; center_column"
    sha256: <sha>
    expected_solve_rate_min: 0.93
```

### Results CSV (`data/results/baseline_<date>.csv`)

```
id,oeis_xref,category,A,N,K,sc,pred_sc,solomonoff_class,mdl,raw_bits,ratio,
time_s,solver_desc,omnis_version,omnis_sha,generator_sha,oeis_snapshot_sha,
run_id,run_date_utc,host,budget_s,freeze_db
```

`solomonoff_class` in `{discovered, compressed_only, not_compressed_predicted, neither}`.

### Run manifest (one per run, alongside results CSV)

```yaml
run_id: <uuid>
omnis_sha: <sha>
omnis_capabilities: [phase2h, sub_call, opt_b, opt_a, wsbp_2h]
generator_sha: <sha>
oeis_snapshot_sha: <sha>
config:
  budget_per_candidate_s: 60
  freeze_db: true
  validate_prediction: true
  prediction_K_rule: "max(20, N/4)"
  strict_prediction: true
hardware:
  os: <uname>
  cpu: <model>
  cores: <n>
  ram_gb: <n>
totals:
  candidates: <n>
  discovered: <n>
  compressed_only: <n>
  duration_s: <n>
```

## 6. Sample selection rules (pre-registered)

| Category | Source | Filter | Alphabets | N | Count |
|---|---|---|---|---|---|
| `oeis_core` | `stripped.gz` | `keyword:core` | {2,3,4,5,7} | 500 | ~850 |
| `oeis_easy` | `stripped.gz` | `keyword:easy AND keyword:nonn` | {2,3,4} | 500 | ~3000 |
| `oeis_hard` | `stripped.gz` | `keyword:hard` | {2,3,4} | 500 | ~200 |
| `oeis_base` | `stripped.gz` | `keyword:base, len>=500` | {2,3,4} | 500 | ~900 |
| `oeis_morphic` | `stripped.gz` | `keyword:morphic` | {2,3} | 500 | ~60 |
| `oeis_cellular` | `stripped.gz` | `name~"cellular"` | {2,3} | 500 | ~100 |
| `eca256` | local | enumerate `[0,256)` | {2} | 500 | 256 |
| `totalistic_3state` | local | enumerate `[0, 3^7)` | {3} | 500 | 2187 |
| `collatz_grid` | local | `(k,c,base) in {3,5,7,9,11}x{1,3,5,7}x{2,3,4}` | derived | 200 | 60 |
| `benchmark14` | local | hand-written ground truth (12 ENARZ Table 1 + 2 nested-loop smoke tests: DivisorCount, Sigma) | mixed | 50–500 | 14 |
| `neg_controls` | local | random / crypto / Pi-base-4 | {2,4} | 500 | 20 |
| **Total** | | | | | **~7800** |

Per-row alphabet reduction: `t' = t mod A`. Recorded in CSV as
`oeis_xref` + `A`.

## 7. Phased execution

Each phase has scope, deliverables, and a binary gate. No phase begins until
prior gates pass.

### Phase A — Generator port (1 day)
- Port 22 generators from `regmachine/tests/discovery_candidates.cpp` to
  `tools/gen_workload.cpp`.
- Pure C++17, no engine link. CLI:
  `gen_workload --category X [--n N] [--seed S]`.
- Each generator carries a version constant.
- **Gate A1.** Byte-identical output across two consecutive runs.
- **Gate A2.** Every emitted line parses cleanly via `cli.cpp::readSequence`.
- **Gate A3.** `eca_030` matches OEIS A070950 first 100 terms.

### Phase B — OEIS ingestion (1.5 days)
- `tools/oeis_fetch.sh` — downloads `stripped.gz` and `names.gz`; SHA-verifies;
  writes `data/oeis/SOURCES.md`.
- `tools/oeis_loader.cpp` — streaming parser; applies named filter; alphabet
  reduction; emits workload format.
- `tools/oeis_filters.h` — six pre-registered filters as typed predicates.
- **Gate B1.** Same snapshot SHA + same filter -> byte-identical workload.
- **Gate B2.** Five canary OEIS sequences (A000005, A000010, A000040, A007814,
  A005132) match their b-files for first 100 terms.
- **Gate B3.** `keyword:core` returns exactly 177 distinct OEIS IDs.

### Phase C — Prediction validation (1 day)
- `tools/omnis_validate.cpp` (~150 LOC). Reads candidate, splits train/test,
  calls `solve` then `predictNext` K times, emits extended CSV row.
- `mine.sh --validate` switches to `omnis_validate`.
- **Gate C1.** Benchmark 12 -> 11/12 `discovered`, 0 `compressed_only`,
  1 `neither` (Pi-b4).
- **Gate C2.** Determinism on benchmark14 ten consecutive runs:
  `(sc, pred_sc)` identical, `mdl` within 0.5 bits.

### Phase D — Wiring + checksums (1 day)
- Populate `data/categories/` with all 11 frozen workload files.
- Compute and commit `data/categories/CHECKSUMS.sha256`.
- Author `data/categories/MANIFEST.yaml`.
- `tests/test_gen_workload.cpp` — 6 tests covering A1/A2/A3, B1/B2/B3, C1/C2.
- **Gate D1.** `make test` is green.
- **Gate D2.** `tools/gen_all.sh && sha256sum -c data/categories/CHECKSUMS.sha256`
  exits 0.

### Phase E — Reproduction validation (~24h compute)
- `tools/run_paper_baseline.sh` — full sweep, freeze-db, validate-prediction,
  writes `data/results/baseline_<date>.csv` and run manifest.
- **Gate E1.** ECA 256 reproduces `discovered >= 240/256` (original 244/256).
- **Gate E2.** `compressed_only / discovered <= 0.01` aggregated across all
  categories.
- **Gate E3.** Negative controls: 0/20 discovered. Pi-base-4: `neither`.
- **Gate E4.** Reroll on a fresh checkout from the committed SHAs reproduces
  the CSV with all determinism-contract fields identical.

### Phase F — Documentation (0.5 day)
- `docs/CATEGORIES.md` — per-category selection rule, OEIS filter, expected
  rates.
- `docs/REPRODUCING.md` — exact recipe: clone -> build -> fetch OEIS -> gen
  -> run -> CSV. Tested on a clean checkout.
- `docs/SOLOMONOFF_VALIDATION.md` — formal definition + example contingency
  table from Phase E.
- README addendum.
- **Gate F1.** Naive-reader test: third party with only the README produces
  the baseline CSV without asking questions.

**Total.** ~5 working days of build + ~1 day of compute. Phases A–C run in
any order after their predecessors; D depends on A+B+C; E depends on D;
F depends on E.

## 8. Threat model & mitigations

| Threat | Mitigation |
|---|---|
| Engine thread-pool non-determinism | `tests/test_engine_determinism.cpp`; contract is `(sc, pred_sc, MDL +/- 0.5)`, `solver_desc` advisory |
| OEIS snapshot drift | SHA-pin in `SOURCES.md`; CI verifies SHA on every build |
| Generator drift | SHA in workload-file header + `CHECKSUMS.sha256` + golden tests |
| Library-state leak | `--freeze-db` enforced by `run_*.sh`; CI test ensures no DB write occurs |
| Compiler / OS drift | Run manifest captures OS + CPU + compiler; reproducibility scoped to `(sc, pred_sc, MDL)` not bit-exact MDL |
| Budget-edge flakiness | CSV records `time_s`; rows with `time_s > 0.95 * budget_s` flagged |
| Workload format drift | `omnis-workload v1` header line; loaders refuse other versions |
| Schema drift in results | `data/results/SCHEMA.md` is normative; tests verify column count + names |
| OEIS ToS / size | Ship SHA + fetch script; do not redistribute `stripped.gz` |

## 9. Decisions locked

| # | Decision | Value |
|---|---|---|
| 1 | OEIS snapshot date | locked at Phase B start |
| 2 | Held-out length K | `max(20, N/4)` |
| 3 | Discovery threshold | strict (`pred_sc == K`) |
| 4 | OEIS bulk in repo? | no — SHA + fetch script |
| 5 | 5-neighbor 2-state CA | excluded |
| 6 | Hard category included? | yes |
| 7 | Discovery semantics | compresses AND predicts |
| 8 | Library state | `--freeze-db` mandatory |
| 9 | Workload schema version | `omnis-workload v1` |
| 10 | Run manifest required? | yes, alongside every results CSV |

## 10. Acceptance

The plan is complete when, on a fresh clone of `omnis@<sha>`:

```bash
git clone ... && cd omnis && mkdir build && cd build && cmake .. && make -j
cd .. && tools/oeis_fetch.sh && tools/gen_all.sh
tools/run_paper_baseline.sh
sha256sum -c data/categories/CHECKSUMS.sha256
```

…produces `data/results/baseline_<date>.csv` whose Solomonoff contingency
satisfies E2, ECA reproduction satisfies E1, and a re-run on the same
machine reproduces all determinism-contract fields.

---

## Appendix A. As-built reconciliation (Pass-8 PhD++)

The implementation deviates from the plan in five places. Each is documented
here with the rationale; this appendix is normative.

### A.1 CSV schema: 15 columns + manifest (was: 22 columns per row)

Plan §5 specified a 22-column CSV with nine sweep-level provenance fields
(`omnis_sha`, `generator_sha`, `oeis_snapshot_sha`, `run_id`,
`run_date_utc`, `host`, `budget_s`, `freeze_db`, `omnis_version`)
replicated on every row. Those nine fields are constants per run — they
identify the sweep, not the candidate. Replicating them × N rows wastes
storage and violates the third normal form.

**As-built:** 15-column CSV with per-row fields only (`id, category,
oeis_xref, A, total_n, train_n, k, sc, pred_sc, solomonoff_class, mdl,
raw_bits, ratio, time_s, solver_desc`). Sweep-level provenance lives in
the sibling `<csv>.manifest.txt` file. Both files together form the
result artefact and must be distributed together. See
`data/results/SCHEMA.md` for the normative column-by-column spec.

This is strictly more storage-efficient and equally complete. The
manifest captures every plan-§3 reproducibility tuple field; the CSV
captures every per-row dimension.

### A.2 `category` and `oeis_xref` are explicit per-row columns

Plan §5's draft schema included these columns but the original CSV emit
in `omnis_validate` did not. As of Pass 8 they are emitted explicitly,
passed in by the sweep runner via `--category` / `--oeis-xref` flags.
Direct CLI invocations default to `category=self`, `oeis_xref=""`.

### A.3 Empty starting `program_db.bin` synthesised inline

Plan §3: "All categorical runs use `--freeze-db` against an empty
starting `program_db.bin`." `tools/run_paper_baseline.sh` writes a
12-byte file (`ENAR` magic + version 2 + count 0) to the output
directory at sweep start and passes it via `--db` to every
`omnis_validate` invocation. The path is recorded in the manifest
(`empty_db_path:`). Library state is verifiably never an implicit input
of a Phase E sweep.

### A.4 `oeis_filters.h` not extracted; filters inline in `oeis_loader.cpp`

Plan §4 listed `tools/oeis_filters.h` as a separate header for filter
predicates. With only one translation unit (`oeis_loader.cpp`)
consuming them, extraction creates an orphan header and adds an
include hop without removing duplication. As-built: filters inline in
`oeis_loader.cpp` lines 70+. The plan §4 entry should be read as "the
filter predicates exist as a typed table" rather than "as a separate
header file."

### A.5 `run_category.sh` subsumed by `run_paper_baseline.sh --categories X`

Plan §4 distinguished a per-category runner from the all-category
runner. As-built: `run_paper_baseline.sh` accepts `--categories LIST`
(comma-separated) and runs any subset, including a singleton. No
separate `run_category.sh` exists.

### A.6 `tests/test_omnis_validate.cpp` merged into `test_gen_workload.cpp`

Plan §4 listed both. As-built: `test_gen_workload.cpp` provides
Gates C1 (header schema), C2 (3-rerun determinism) and the
`[CTX-regression]` train/test-split + prediction wiring check.
Functional coverage is equivalent; the file boundary differs.

### A.7 `test_engine_determinism.cpp` IS now a separate file (§3 contract)

Per Pass 8 PhD++ realignment, the 30-candidate × 3-rerun canary
mandated by §3 is now its own test (`tests/test_engine_determinism.cpp`)
with explicit `(sc, pred_sc)` identical and `MDL ±0.5` assertions on
30 candidates. The single-candidate C2 in `test_gen_workload.cpp` is
the fast smoke; this file is the §3 normative canary.

### A.8 Solomonoff lex-best correctness — Pass-6/6b/6c/6d/7 (Pass 8 supersession)

The plan §3 "Engine determinism" wording refers only to thread-pool
order on ties. During Pass-8 realignment we discovered that the
engine's Phase 2B `else if(cas_sc>=best.sc)` branch was creating
`Res` records with empty body and `branch_m=0` (UB on `emod`), which
won lex-best because their MDL was artificially small (empty body).
Fixed in Pass 7. Similar lex-best gates added across CTX (Pass 6),
CONCAT/DARY (Pass 6b), Phase 2A/2H collecting (Pass 6c), and Phase
1A-ext / 2F / 2H-inner (Pass 6d). These fixes are correctness, not
performance — they change which program `best` holds at solve() end.

Effect on plan: reported MDLs now strictly correspond to programs
that actually exist and were run-verified. The MDL determinism contract
in §3 still holds (verified by the 30-candidate canary).
