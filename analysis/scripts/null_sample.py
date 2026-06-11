#!/usr/bin/env python3
"""
null_sample.py  --  stratified random sample for the gate-null sweep.

Picks N sequences per population (defaults: S1=37, S2=20, S3=76, S4=168, total
301 ≈ the 300 the prompt asks for, rounded up to fully consume the S4 share).
Uses a fixed seed so the sample reproduces across reruns.

Filters out sequences whose multiset entropy is 0 (constant sequences) - the
shuffle is identity-equivalent on those, so they cannot inform a null
calibration. The methodology requires "order to destroy"; constant sequences
have none.

Outputs analysis/null_sample.csv with columns:
    sample_idx, population, source_csv, original_id, category,
    alphabet_size, total_n, multiset_H
"""

import csv, math, os, sys, random
from collections import Counter

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")
OUT_PATH     = os.path.join(ANALYSIS_DIR, "null_sweep", "null_sample.csv")
SEED         = 42

# Sample counts proportional to population sizes (488/256/983/2187 ≈ 3,914),
# scaled to ~300 trials total with the floor at the smallest population.
STAGES = [
    ("S1", "baseline_20260511T091836Z.csv", False, 37),
    ("S2", "baseline_20260513T232442Z.csv", False, 20),
    ("S3", "baseline_20260514T024454Z.csv", True,  76),
    ("S4", "baseline_20260520T155701Z.csv", False, 168),
]
DROP_XREF = {"A000069", "A000120", "A001969", "A002113"}
CAT_DIR   = os.path.join(REPO_ROOT, "data", "categories")


def index_category(cat):
    """Read data/categories/<cat>.txt → {id: terms[]}."""
    path = os.path.join(CAT_DIR, f"{cat}.txt")
    idx = {}
    if not os.path.exists(path):
        return idx
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            N = int(parts[2])
            idx[parts[0]] = list(map(int, parts[3:3 + N]))
        except ValueError:
            continue
    return idx


def multiset_entropy(terms):
    n = len(terms)
    if n == 0:
        return 0.0
    c = Counter(terms)
    return -sum((v / n) * math.log2(v / n) for v in c.values() if v)


def main():
    rng = random.Random(SEED)
    rows = []
    sample_idx = 0
    cat_cache = {}
    excluded = {}  # stage -> count of H=0 candidates skipped
    for stage, fname, dedupe, n_pick in STAGES:
        src = os.path.join(RESULTS_DIR, fname)
        if not os.path.exists(src):
            sys.exit(f"missing baseline CSV: {src}")
        candidates = []
        n_const = 0
        for r in csv.DictReader(open(src)):
            if dedupe and r.get("category") == "oeis_base" and r.get("oeis_xref") in DROP_XREF:
                continue
            cat = r["category"]
            if cat not in cat_cache:
                cat_cache[cat] = index_category(cat)
            terms = cat_cache[cat].get(r["id"])
            if terms is None:
                sys.exit(f"{stage}: id {r['id']} not in {cat}.txt - cannot compute multiset H")
            H = multiset_entropy(terms)
            if H == 0.0:
                n_const += 1
                continue
            r["_multiset_H"] = H
            candidates.append(r)
        excluded[stage] = n_const
        if len(candidates) < n_pick:
            sys.exit(f"{stage}: only {len(candidates)} non-constant candidates, need {n_pick}")
        picked = rng.sample(candidates, n_pick)
        picked.sort(key=lambda r: r["id"])
        for r in picked:
            rows.append({
                "sample_idx":     sample_idx,
                "population":     stage,
                "source_csv":     fname,
                "original_id":    r["id"],
                "category":       r["category"],
                "alphabet_size":  r["A"],
                "total_n":        r["total_n"],
                "multiset_H":     f"{r['_multiset_H']:.4f}",
            })
            sample_idx += 1

    with open(OUT_PATH, "w", newline="") as out:
        w = csv.DictWriter(out, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    print(f"wrote {OUT_PATH}")
    print(f"  total picked: {len(rows)}")
    c = Counter(r["population"] for r in rows)
    for s in ("S1","S2","S3","S4"):
        print(f"    {s}: {c[s]} (planned {dict((x[0],x[3]) for x in STAGES)[s]})  "
              f"[excluded {excluded[s]} constants from candidate pool]")
    print(f"  seed: {SEED}")
    print(f"  exclusion rule: multiset entropy H(terms) > 0 (constant sequences dropped)")

if __name__ == "__main__":
    main()
