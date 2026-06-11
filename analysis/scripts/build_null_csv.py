#!/usr/bin/env python3
"""
build_null_csv.py  --  convert raw engine output (null_raw.csv) into the
4-column null.csv that analyze_omnis.py reads.

Output columns: null_type, population, compressed, predicted

  compressed = 1 iff sc == train_n  (the compression gate)
  predicted  = 1 iff pred_sc == k   (the prediction gate)

A trial is counted as a "discovery" only when both are 1 - that's the
Solomonoff cell.  This script *only* assembles columns from data; it never
fabricates an outcome.
"""

import csv, os, sys

ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SWEEP_DIR    = os.path.join(ANALYSIS_DIR, "null_sweep")
RAW          = os.path.join(SWEEP_DIR, "null_raw.csv")
SAMPLE       = os.path.join(SWEEP_DIR, "null_sample.csv")
AUDIT        = os.path.join(SWEEP_DIR, "null_workload_audit.csv")
OUT          = os.path.join(ANALYSIS_DIR, "null.csv")
NULL_TYPE   = "shuffled"

def main():
    for p in (RAW, SAMPLE, AUDIT):
        if not os.path.isfile(p):
            sys.exit(f"missing {p}")
    # original_id  ->  population  (from sample)
    pop = {r["original_id"]: r["population"] for r in csv.DictReader(open(SAMPLE))}
    # shuffled_id  ->  original_id  (from audit)
    backmap = {r["shuffled_id"]: r["original_id"] for r in csv.DictReader(open(AUDIT))}

    n_total = n_disc = n_comp = n_pred = 0
    rows = []
    for r in csv.DictReader(open(RAW)):
        sid = r["id"]
        orig = backmap.get(sid)
        if not orig:
            sys.exit(f"id {sid} from null_raw.csv not found in workload audit")
        population = pop[orig]
        sclass = r["solomonoff_class"]
        compressed = 1 if sclass in ("discovered", "compressed_only") else 0
        predicted  = 1 if sclass in ("discovered", "not_compressed_predicted") else 0
        rows.append({
            "null_type":  NULL_TYPE,
            "population": population,
            "compressed": compressed,
            "predicted":  predicted,
        })
        n_total += 1
        n_comp  += compressed
        n_pred  += predicted
        if compressed and predicted:
            n_disc += 1

    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["null_type","population","compressed","predicted"])
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {OUT}")
    print(f"  trials:              {n_total}")
    print(f"  discoveries:         {n_disc}  (rate {n_disc/n_total if n_total else 0:.5f})")
    print(f"  compressed-only:     {n_comp - n_disc}")
    print(f"  predicted-only:      {n_pred - n_disc}")

if __name__ == "__main__":
    main()
