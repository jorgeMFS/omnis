# analysis

Everything behind the paper's figures and tables. Every number in the
manuscript traces back to a script in this directory, and every script
reads only the deposited data under `data/`. Nothing here is hand-edited.

## Layout

```
analysis/
  analyze_omnis.py        main analysis script (scaling regression,
                          gate false-acceptance bound, null tabulation)
  discoveries.csv         2,383 discovered rows, one per discovery
  null.csv                321 null trials, gate outcomes in 4 columns
  sc_input.csv            3,914-row per-candidate table for SI Appendix C

  scripts/                Python utilities (see scripts/README.md)
    build_discoveries_csv.py   builds discoveries.csv from the baselines
    build_null_csv.py          builds null.csv from the sweep artefacts
    build_sc_input.py          builds sc_input.csv from the baselines
    generate_sc_tables.py      emits the SI Appendix C tables as LaTeX
    null_sample.py             stratified sample for the null sweep
    null_make_workload.py      deterministic shuffle of the sample
    null_topup_s2.py           S2 top-up at high entropy
    run_null_sweep.py          engine driver for the null sweep
    null_tiers.py              entropy-stratified null table
    alphabet_separation.py     alphabet-scaling tests (McNemar)
    scaling_censored.py        censored fit and model comparison
                               (not used by the current manuscript)

  null_sweep/             null-calibration sweep artefacts (see its README)
    null_raw.csv               engine output, one row per trial
    null_sample.csv            the sample, with multiset entropy per row
    null_shuffled.workload     engine-format workload, 321 lines
    null_workload_audit.csv    per-trial seeds and multiset hashes
    null_sweep.log             run log, about 48 hours of wall time

  figures/                R scripts and rendered paper figures
  results/                final outputs: companion figures, SI tables,
                          statistical side-cars
```

The figure scripts produce the paper figures directly: fig6 is the
compression-prediction landscape (Fig. 2), fig3 the resolution-time
strip (Fig. 3), fig5 the ECA grid (Fig. S1), fig8 the totalistic-CA
heatmap (Fig. S2), and fig4 the discovery time against description
length, colored by solver family, with the 10 s collection window and
the 600 s budget marked (Fig. S3). There is also one R companion to
`analyze_omnis.py`. `fig4_si_scatter_render.R` is not used by the
current manuscript and is kept for reference. Each script writes a PDF, a
PNG preview and a data side-car next to itself.

`results/` holds `sc_tables.tex` (the seven SI Appendix C tables,
paste-ready), the R-rendered companion figure
(`fig_scaling_cost_vs_length`), and the CSV
side-cars from the statistical scripts (`null_tiers.csv`,
`alphabet_separation.csv`, `alphabet_cohorts.csv`,
`scaling_censored.csv`).

`scripts/scaling_censored.py` and its side-car `results/scaling_censored.csv`
are not used by the current manuscript. The current SI reports the
relation between discovery time and description length descriptively
(Spearman correlation, 10 s floor, 600 s cap) and no longer reports
censored fits or model comparisons. The script is kept for reference.

## Which file backs which table or figure

| paper item | data | script |
|---|---|---|
| Fig. 1 (architecture) | -- | drawn, no data |
| Fig. 2 / fig6 (landscape) | `figures/fig6_data.csv` | `figures/fig6_render.R` |
| Fig. 3 (cliff strip) | `figures/fig3_strip_data.csv` | `figures/fig3_render.R` |
| Fig. S1 / fig5 (ECA grid) | `figures/fig5_data.csv` | `figures/fig5_render.R` |
| Fig. S2 / fig8 (S4 heatmap) | `figures/fig8_data.csv` | `figures/fig8_render.R` |
| Fig. S3 / fig4 (time vs length) | `figures/fig4_data.csv` | `figures/fig4_render.R` |
| Tables SC-1..SC-7 | `sc_input.csv` | `scripts/generate_sc_tables.py` |
| SC-3 hard subset + inversions | `results/alphabet_cohorts.csv` | `scripts/alphabet_cohorts.py` |
| Section 3.3 slopes | `discoveries.csv` | `analyze_omnis.py` |
| Section 3.5 McNemar | `data/results/baseline_*.csv` | `scripts/alphabet_separation.py` |
| Null table (tab:snull) | `null_sweep/null_raw.csv` | `scripts/null_tiers.py` |
| Gate false-acceptance bound (SI-B) | `discoveries.csv`, `sc_input.csv` | `analyze_omnis.py` |

## Reproducing

All scripts resolve their paths relative to their own location, so the
working directory does not matter. Re-emitting `discoveries.csv`,
`null.csv` and `sc_input.csv` is bit-identical run to run.

```bash
# Discovery-side tables (seconds; reads data/results/ baselines)
python3 scripts/build_discoveries_csv.py
python3 scripts/build_sc_input.py

# Null sweep (the only expensive step: about 50 h of engine time)
python3 scripts/null_sample.py
python3 scripts/null_topup_s2.py
python3 scripts/null_make_workload.py
python3 scripts/run_null_sweep.py
python3 scripts/build_null_csv.py

# Statistics
python3 analyze_omnis.py --discoveries discoveries.csv --null null.csv --outdir results/
python3 scripts/null_tiers.py
python3 scripts/alphabet_separation.py

# SI Appendix C tables
python3 scripts/generate_sc_tables.py --results sc_input.csv > results/sc_tables.tex

# Figures (the R companion overwrites the matplotlib version in results/)
cd figures && for f in *_render.R; do Rscript "$f"; done
```

Python dependencies: numpy, pandas, scipy, statsmodels, lifelines,
scikit-learn. R dependencies: ggplot2, patchwork, ggrepel.

Every build script asserts the published invariants before writing its
output (corpus 3,914; discoveries 2,383; gate split 2,383/17/27/1,487;
per-population discoveries 120/244/336/1,683) and aborts on mismatch,
so a stale or edited deposition cannot silently produce wrong tables.

## Provenance

| file | produced by | consumed by |
|---|---|---|
| `discoveries.csv` | `scripts/build_discoveries_csv.py` | `analyze_omnis.py`, `figures/fig_scaling_render.R` |
| `null.csv` | `scripts/build_null_csv.py` | `analyze_omnis.py` |
| `sc_input.csv` | `scripts/build_sc_input.py` | `scripts/generate_sc_tables.py` |
| `null_sweep/null_raw.csv` | `scripts/run_null_sweep.py` | `scripts/build_null_csv.py`, `scripts/null_tiers.py` |
| `figures/fig4_data.csv` | deposited; no script in this directory writes it | `figures/fig4_render.R` (reads it only) |
| other `figures/fig*_data.csv` | each figure render script (fig3, fig4_si_scatter, fig5, fig6, fig8) | the same script |

The inputs all of this rests on are the four baseline sweep CSVs in
`data/results/` (one per population, with their run manifests) and the
workload files in `data/categories/`. Neither is written by anything in
this directory.

## Two notes a careful reader will want

The canonical source for the entropy-stratified null table and the
H ≥ 0.7 headline is `scripts/null_tiers.py`. The two diagnostic counters
that `analyze_omnis.py` prints under the names "compress-only" and
"predict-only" are gate totals (discoveries plus exclusive-only), not
the exclusive counts. The exclusive counts come from `build_null_csv.py`
and `null_tiers.py`.

The null sweep excludes constant sequences (multiset entropy H = 0)
before sampling, because shuffling a constant sequence is the identity
and such a trial cannot inform a permutation null. The headline cohort
is restricted further to H ≥ 0.7, where the ablation genuinely destroys
order: 201 trials, zero discoveries, 95% Clopper-Pearson upper bound
1.48%. The six discoveries observed in lower-entropy tiers are correct
detections of trivially recoverable structure, not gate false alarms;
the per-tier breakdown is in `results/null_tiers.csv`.
