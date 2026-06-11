#!/usr/bin/env python3
"""
alphabet_cohorts.py -- emit the two per-sequence lists the SI references for
the alphabet analysis (Section 3.5 / Table SC-3):

  hard_subset:  the 26 oeis_hard sequences encoded at all three bases
                (2, 3, 4), with their gate class per base and a discordant
                flag (binary outcome differs from ternary) -- the pairs
                behind the exact McNemar test.
  inversion:    the 30 (of 261) multi-alphabet S3 sequences that invert the
                difficulty trend: some larger base is discovered while a
                smaller base is not.

Reads sc_input.csv and the S1 baseline; writes results/alphabet_cohorts.csv.
"""
import csv, os, sys
from collections import defaultdict

ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SC_INPUT = os.path.join(ANALYSIS_DIR, "sc_input.csv")
S1_BASE  = os.path.join(ANALYSIS_DIR, "..", "data", "results",
                        "baseline_20260511T091836Z.csv")
OUT      = os.path.join(ANALYSIS_DIR, "results", "alphabet_cohorts.csv")

def main():
    # hard subset from the S1 baseline (category column lives there)
    hard = defaultdict(dict)
    for r in csv.DictReader(open(S1_BASE)):
        if r["category"] != "oeis_hard":
            continue
        hard[r["oeis_xref"]][int(r["A"])] = r["solomonoff_class"]
    hard3 = {k: v for k, v in hard.items() if set(v) >= {2, 3, 4}}
    discordant = [k for k in hard3
                  if (hard3[k][2] == "discovered") != (hard3[k][3] == "discovered")]

    # inversion cohort: multi-alphabet S3 sequences
    groups, pops = defaultdict(dict), {}
    for r in csv.DictReader(open(SC_INPUT)):
        b = r["alphabet_base"]
        if b in ("", "NA", "nan"):
            continue
        groups[r["base_seq_id"]][int(float(b))] = r["gate_class"]
        pops[r["base_seq_id"]] = r["population"]
    cohort = [k for k, v in groups.items() if len(v) >= 2 and pops[k] == "S3"]
    inversions = []
    for sid in sorted(cohort):
        v = groups[sid]
        bs = sorted(v)
        pairs = [(b1, b2) for i, b1 in enumerate(bs) for b2 in bs[i+1:]
                 if v[b1] != "discovered" and v[b2] == "discovered"]
        if pairs:
            inversions.append((sid, bs, [v[b] for b in bs], pairs))

    # the published counts; abort on mismatch so a stale deposition can't lie
    checks = [("hard subset size", len(hard3), 26),
              ("binary discoveries", sum(1 for v in hard3.values() if v[2] == "discovered"), 7),
              ("A=3 discoveries", sum(1 for v in hard3.values() if v[3] == "discovered"), 0),
              ("A=4 discoveries", sum(1 for v in hard3.values() if v[4] == "discovered"), 0),
              ("discordant pairs", len(discordant), 7),
              ("multi-alphabet S3 cohort", len(cohort), 261),
              ("inversions", len(inversions), 30)]
    ok = True
    for name, got, exp in checks:
        flag = "OK " if got == exp else "!! "
        print(f"  {flag}{name}: got {got}, expected {exp}")
        ok &= (got == exp)
    if not ok:
        sys.exit("ABORT: published invariants not reproduced")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cohort", "sequence_id", "bases_present",
                    "gate_class_per_base", "note"])
        for sid in sorted(hard3):
            v = hard3[sid]
            w.writerow(["hard_subset", sid, "2;3;4",
                        f"{v[2]};{v[3]};{v[4]}",
                        "discordant" if sid in discordant else ""])
        for sid, bs, classes, pairs in inversions:
            w.writerow(["inversion", sid, ";".join(map(str, bs)),
                        ";".join(classes),
                        "easier at larger base: "
                        + ",".join(f"{a}->{b}" for a, b in pairs)])
    print(f"wrote {OUT} ({len(hard3)} hard + {len(inversions)} inversion rows)")

if __name__ == "__main__":
    main()
