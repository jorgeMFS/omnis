#!/usr/bin/env python3
"""
null_topup_s2.py  --  one-shot top-up for the S2 cohort at H >= 0.7.

The base stratified sample (null_sample.csv) has 20 S2 sequences picked
proportionally, of which only ~10 land in the H >= 0.7 cohort that we use
for the headline FAP calibration. To make the per-population CP non-vacuous,
we draw an additional 20 S2 sequences sampled directly from the H >= 0.7
sub-pool, excluding any id already present in the base sample.

Appends to null_sample.csv (sample_idx 301..320), with reproducible seed.
The audit + workload regeneration are then done by re-running
null_make_workload.py over the augmented null_sample.csv.
"""

import csv, math, os, random, sys
from collections import Counter

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CAT_DIR      = os.path.join(REPO_ROOT, "data", "categories")
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")
SAMPLE_CSV   = os.path.join(ANALYSIS_DIR, "null_sweep", "null_sample.csv")

S2_CSV  = "baseline_20260513T232442Z.csv"
S2_CAT  = "eca256"
N_TOPUP = 20
THRESHOLD = 0.7
TOPUP_SEED = 4242  # distinct from the base sample's seed=42


def multiset_entropy(terms):
    n = len(terms)
    if n == 0: return 0.0
    c = Counter(terms)
    return -sum((v / n) * math.log2(v / n) for v in c.values() if v)


def main():
    # Index eca256 category file (id -> terms)
    idx = {}
    for line in open(os.path.join(CAT_DIR, f"{S2_CAT}.txt")):
        if line.startswith("#") or not line.strip(): continue
        parts = line.split()
        if len(parts) < 4: continue
        idx[parts[0]] = list(map(int, parts[3:3 + int(parts[2])]))

    # Already-sampled S2 ids
    base_rows = list(csv.DictReader(open(SAMPLE_CSV)))
    already_s2 = {r["original_id"] for r in base_rows if r["population"] == "S2"}
    print(f"  S2 ids already in base sample: {len(already_s2)}")

    # Eligible pool: S2 baseline rows with H >= THRESHOLD and not already sampled
    eligible = []
    for r in csv.DictReader(open(os.path.join(RESULTS_DIR, S2_CSV))):
        sid = r["id"]
        if sid in already_s2:
            continue
        if sid not in idx:
            sys.exit(f"{sid} not in {S2_CAT}.txt")
        H = multiset_entropy(idx[sid])
        if H >= THRESHOLD:
            r["_H"] = H
            eligible.append(r)

    print(f"  S2 eligible at H>={THRESHOLD} (excluding base sample): {len(eligible)}")
    if len(eligible) < N_TOPUP:
        sys.exit(f"only {len(eligible)} eligible, need {N_TOPUP}")

    rng = random.Random(TOPUP_SEED)
    picked = rng.sample(eligible, N_TOPUP)
    picked.sort(key=lambda r: r["id"])

    next_idx = max(int(r["sample_idx"]) for r in base_rows) + 1
    new_rows = []
    for r in picked:
        new_rows.append({
            "sample_idx":     next_idx,
            "population":     "S2",
            "source_csv":     S2_CSV,
            "original_id":    r["id"],
            "category":       r["category"],
            "alphabet_size":  r["A"],
            "total_n":        r["total_n"],
            "multiset_H":     f"{r['_H']:.4f}",
        })
        next_idx += 1

    # Append to null_sample.csv (preserve header + base rows)
    fieldnames = list(base_rows[0].keys())
    with open(SAMPLE_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(base_rows)
        w.writerows(new_rows)

    print(f"  appended {len(new_rows)} S2 top-up rows to {SAMPLE_CSV}")
    print(f"  top-up seed: {TOPUP_SEED}")
    print(f"  top-up id range: {picked[0]['id']} .. {picked[-1]['id']}")
    print(f"  new total sample size: {len(base_rows) + len(new_rows)}")

if __name__ == "__main__":
    main()
