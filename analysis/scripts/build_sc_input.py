#!/usr/bin/env python3
"""
build_sc_input.py -- combine the four baseline CSVs into the single per-candidate
results file that generate_sc_tables.py expects.

Read-only over data/results/baseline_*.csv. Applies the S3 dedupe (drop oeis_base
rows whose oeis_xref ∈ {A000069, A000120, A001969, A002113}) and the schema
renames the SC generator requires:

    baseline column        →   sc_input column
    -------------------       ------------------------
    id                     →   candidate_id
    (derived from file)    →   population         (S1/S2/S3/S4)
    solomonoff_class       →   gate_class         (with "not_compressed_predicted"
                                                   → "predicted_only")
    solver_desc            →   solver_family      (first space-delimited word)
    mdl                    →   mdl_bits
    train_n * log2(A)      →   train_bits         (computed)
    raw_bits               →   raw_bits           (same)
    k                      →   holdout_len
    A                      →   alphabet_size
    time_s                 →   discovery_time_s
    (parse id "A####_aN")  →   alphabet_base      (N for OEIS multi-alphabet, else "")
    (parse id "A####_aN")  →   base_seq_id        ("A####" for OEIS, else id)

Output: <ANALYSIS_DIR>/sc_input.csv  (parallel to discoveries.csv and null.csv).
"""

import csv
import math
import os
import re
import sys
from collections import Counter

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")
OUT_PATH     = os.path.join(ANALYSIS_DIR, "sc_input.csv")

# Baselines and their stage labels. S3 has the dedupe applied here, identically
# to the rule already used by build_discoveries_csv.py / fig6_render.R / etc.
STAGES = [
    ("S1", "baseline_20260511T091836Z.csv", False),
    ("S2", "baseline_20260513T232442Z.csv", False),
    ("S3", "baseline_20260514T024454Z.csv", True),
    ("S4", "baseline_20260520T155701Z.csv", False),
]
DROP_XREF = {"A000069", "A000120", "A001969", "A002113"}

GATE_RENAME = {
    "discovered":               "discovered",
    "compressed_only":          "compressed_only",
    "not_compressed_predicted": "predicted_only",  # ← the SC script's name
    "neither":                  "neither",
}

OEIS_ID_RE = re.compile(r"^(A\d+)_a(\d+)$")

OUT_COLS = [
    "candidate_id", "population", "gate_class", "solver_family",
    "mdl_bits", "train_bits", "raw_bits", "holdout_len",
    "alphabet_size", "discovery_time_s",
    "alphabet_base", "base_seq_id",
]


def parse_family(solver_desc):
    if not solver_desc:
        return ""
    head = solver_desc.lstrip('"')
    return head.split(" ", 1)[0]


def parse_oeis(id_):
    m = OEIS_ID_RE.match(id_)
    if not m:
        return "", id_
    return m.group(2), m.group(1)


def main():
    rows_out = []
    expected = {"S1": 488, "S2": 256, "S3": 983, "S4": 2187}
    for pop, fname, dedupe in STAGES:
        src = os.path.join(RESULTS_DIR, fname)
        if not os.path.exists(src):
            sys.exit(f"missing baseline CSV: {src}")
        n_pop = 0
        for r in csv.DictReader(open(src)):
            if dedupe and r.get("category") == "oeis_base" and r.get("oeis_xref") in DROP_XREF:
                continue
            A = int(r["A"])
            train_n = int(r["train_n"])
            train_bits = train_n * math.log2(A) if A > 1 else 0.0
            alphabet_base, base_seq_id = parse_oeis(r["id"])
            cls_in = r["solomonoff_class"]
            cls_out = GATE_RENAME.get(cls_in)
            if cls_out is None:
                sys.exit(f"unknown gate class '{cls_in}' in {fname} row {r['id']}")
            rows_out.append({
                "candidate_id":     r["id"],
                "population":       pop,
                "gate_class":       cls_out,
                "solver_family":    parse_family(r["solver_desc"]),
                "mdl_bits":         r["mdl"],
                "train_bits":       f"{train_bits:.6f}",
                "raw_bits":         r["raw_bits"],
                "holdout_len":      r["k"],
                "alphabet_size":    r["A"],
                "discovery_time_s": r["time_s"],
                "alphabet_base":    alphabet_base,
                "base_seq_id":      base_seq_id,
            })
            n_pop += 1
        if n_pop != expected[pop]:
            sys.exit(f"{pop}: got {n_pop} rows, expected {expected[pop]}")
        print(f"  {pop}: {n_pop}", file=sys.stderr)

    with open(OUT_PATH, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=OUT_COLS)
        w.writeheader()
        w.writerows(rows_out)

    # Tally invariants for human inspection
    cls_counts = Counter(r["gate_class"] for r in rows_out)
    pop_disc = Counter(r["population"] for r in rows_out if r["gate_class"] == "discovered")
    print(f"\nwrote {OUT_PATH} ({len(rows_out)} rows)", file=sys.stderr)
    print(f"gate split: {dict(cls_counts)}", file=sys.stderr)
    print(f"per-pop discoveries: {dict(pop_disc)}", file=sys.stderr)


if __name__ == "__main__":
    main()
