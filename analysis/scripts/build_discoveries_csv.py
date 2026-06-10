#!/usr/bin/env python3
"""
build_discoveries_csv.py  --  one-shot builder for analyze_omnis.py input.

Reads the four baseline CSVs from data/results/, filters to rows with
solomonoff_class == "discovered" (applying the S3 dedupe rule so the unified
count matches the manuscript's 2,383 discoveries), and emits the column subset
the analysis script expects:

  time_s, program_bits, population, alphabet_size, holdout_len, nesting_depth

Where:
  * program_bits  = `mdl` column.  Justification: for discovered rows the engine
                    achieves sc == train_n AND pred_sc == k, so the residual is
                    zero by construction and `mdl` reduces to the program
                    description length.  See computeMDL() in src/omnis.cpp.
  * population    = "S1" / "S2" / "S3" / "S4", per the user's stage label.
  * alphabet_size = `A`.
  * holdout_len   = `k`.
  * nesting_depth = integer from solver_desc when the family is NESTED_LOOP
                    (e.g. `"NESTED_LOOP L=4 ..."` -> 4).  NaN otherwise.
                    (kappa is not currently logged per-row by the engine.)

No fabrication: every value is read straight out of the baseline CSV columns.
"""

import csv, re, os, sys

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")

# Source CSVs and their stage labels.
STAGES = [
    ("S1", "baseline_20260511T091836Z.csv", False),
    ("S2", "baseline_20260513T232442Z.csv", False),
    ("S3", "baseline_20260514T024454Z.csv", True),
    ("S4", "baseline_20260520T155701Z.csv", False),
]

# S3 dedupe rule: drop oeis_base rows whose oeis_xref already appears in S1.
DROP_XREF = {"A000069", "A000120", "A001969", "A002113"}

L_RE = re.compile(r"\bL=(\d+)")

def parse_depth(solver_desc):
    """Extract integer L from solver_desc, but only for NESTED_LOOP rows."""
    if not solver_desc:
        return ""
    head = solver_desc.lstrip('"').split(" ", 1)[0]
    if head != "NESTED_LOOP":
        return ""
    m = L_RE.search(solver_desc)
    return m.group(1) if m else ""

def main():
    out_path = os.path.join(ANALYSIS_DIR, "discoveries.csv")
    total = 0
    per_stage = {}
    with open(out_path, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["time_s", "program_bits", "population",
                    "alphabet_size", "holdout_len", "nesting_depth"])
        for stage, fname, dedupe in STAGES:
            src = os.path.join(RESULTS_DIR, fname)
            if not os.path.exists(src):
                sys.exit(f"missing baseline CSV: {src}")
            n_stage = 0
            for r in csv.DictReader(open(src)):
                if dedupe and r.get("category") == "oeis_base" and r.get("oeis_xref") in DROP_XREF:
                    continue
                if r["solomonoff_class"] != "discovered":
                    continue
                w.writerow([
                    r["time_s"],
                    r["mdl"],
                    stage,
                    r["A"],
                    r["k"],
                    parse_depth(r.get("solver_desc", "")),
                ])
                n_stage += 1
            per_stage[stage] = n_stage
            total += n_stage

    print(f"wrote {out_path}")
    print(f"  total discovered rows: {total}  (expected 2,383)")
    for stage in ("S1", "S2", "S3", "S4"):
        print(f"    {stage}: {per_stage[stage]}")
    # Hard gate matching the manuscript's full-corpus count.
    expected = {"S1": 120, "S2": 244, "S3": 336, "S4": 1683}
    for k, v in expected.items():
        if per_stage[k] != v:
            sys.exit(f"MISMATCH {k}: got {per_stage[k]}, expected {v}")
    if total != 2383:
        sys.exit(f"MISMATCH total: got {total}, expected 2,383")
    print("gate PASS")

if __name__ == "__main__":
    main()
