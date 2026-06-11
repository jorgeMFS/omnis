# scripts

Python utilities behind the analysis directory. None of them modify the
engine, the program database, or the baseline sweep results; they read
those and write only into `analysis/`.

## What each script does

Builders, run first:

```bash
python3 build_discoveries_csv.py    # writes ../discoveries.csv from the baselines
python3 build_sc_input.py           # writes ../sc_input.csv from the baselines
```

Null sweep, in order (the engine run is the only expensive step):

```bash
python3 null_sample.py              # stratified sample, constants excluded
python3 null_topup_s2.py            # S2 top-up at high entropy, 301 -> 321 rows
python3 null_make_workload.py       # deterministic shuffle + audit file
python3 run_null_sweep.py           # engine driver, about 50 h
python3 build_null_csv.py           # writes ../null.csv from the raw sweep output
```

Statistics, any order once the builders have run:

```bash
python3 null_tiers.py               # entropy-stratified null table + headline
python3 alphabet_separation.py      # alphabet-scaling rates, McNemar test
python3 alphabet_cohorts.py         # hard-subset + inversion lists (SC-3)
python3 scaling_censored.py         # censored scaling fit, model comparison
python3 generate_sc_tables.py --results ../sc_input.csv   # SI Appendix C LaTeX
```

## Paths

Every script anchors its paths to its own file location, so the working
directory does not matter. The relevant locations are the repo root
(`omnis/`), the analysis directory, `analysis/null_sweep/` for sweep
artefacts, and `data/results/` and `data/categories/` as read-only
inputs. `run_null_sweep.py` expects the `omnis_validate` binary at
`build-cmake/omnis_validate`.

## Determinism

`null_sample.py` samples with seed 42 and `null_topup_s2.py` with seed
4242. `null_make_workload.py` derives each trial's shuffle seed from
the sequence id (`sha256("omnis-null-shuffled" || id)`, first 8 bytes),
so every shuffle is reproducible independently. Re-running the builders
produces bit-identical CSVs.

## Write boundaries

These scripts never write to `data/program_db.bin`, the baseline CSVs in
`data/results/`, or the workload files in `data/categories/`. They write
only `analysis/discoveries.csv`, `analysis/null.csv`,
`analysis/sc_input.csv`, the artefacts in `analysis/null_sweep/`, and
the side-cars in `analysis/results/`.
