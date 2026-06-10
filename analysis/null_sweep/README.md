# null_sweep

Artefacts of the null-calibration sweep: the engine run on shuffled
sequences that calibrates the gate's false-alarm rate.

## Files

`null_raw.csv` is the engine output, one row per trial, in the same
CSV schema as the baseline sweeps (`id, category, oeis_xref, A, total_n,
train_n, k, sc, pred_sc, solomonoff_class, mdl, raw_bits, ratio, time_s,
solver_desc`). 321 rows.

`null_sample.csv` is the stratified sample: 301 rows drawn across the
four populations with constant sequences excluded, plus a 20-row S2
top-up at high entropy, for 321 in total. Each row records the
sequence's multiset entropy.

`null_shuffled.workload` is the engine-format workload, one shuffled
sequence per line. Each trial's permutation is seeded deterministically
from the sequence id, so any single trial can be reproduced in
isolation.

`null_workload_audit.csv` records, per trial, the shuffle seed and the
sha256 of the original and shuffled multisets. The two hashes match on
every row, which proves the shuffle preserved the symbol counts.

`null_sweep.log` is the driver log from the actual run, about 48 hours
of wall time, no engine crashes.

## How these were produced

In order, by the scripts in `../scripts/`: `null_sample.py` (sample),
`null_topup_s2.py` (S2 top-up), `null_make_workload.py` (shuffle +
audit), `run_null_sweep.py` (the engine run). The 4-column `null.csv`
that `analyze_omnis.py` consumes is then built one level up by
`build_null_csv.py`.

## Outcome

Over the strict cohort, where shuffling genuinely destroys order
(multiset entropy at least 0.7): 201 trials, zero discoveries, 95%
Clopper-Pearson upper bound on the false-alarm rate 1.48%. Six
discoveries occur in the low-entropy tiers; they are correct detections
of trivially recoverable structure (sequences a constant program away
from their shuffle), not gate failures. The per-tier table is in
`../results/null_tiers.csv`.
