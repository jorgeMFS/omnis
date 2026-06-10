#!/usr/bin/env python3
"""
alphabet_separation.py — substantiate the §3.5 alphabet-scaling claim
with Fisher's exact test, named subset, named n, and an exact p-value.

The §3.5 paragraph contains two distinct numerical claims about how the
engine's discovery rate scales with alphabet size on OEIS-derived rows:

  (1) Rate progression "54 % binary → 33 % ternary → 27 % in the next"
      — verified against S3 oeis_base (161 sequences at each of A=2/3/4,
      deduplicated, the same population fig6 uses).

  (2) Hardest OEIS subset: binary rate "near thirty percent" collapsing
      to zero at larger alphabets, separation significant beyond 1e-6
      — verified against S1 oeis_hard (26 sequences at each of A=2/3/4),
      via Fisher's exact two-tailed test on a 2x2 contingency table.

Read-only on the baselines; writes a single side-car to
analysis/results/alphabet_separation.csv. Discoveries are counted by the
corpus criterion (solomonoff_class == "discovered"), the same criterion
fixed for the null analysis (build_null_csv.py, null_tiers.py).
"""

import csv
import os
import re
import sys
from collections import defaultdict
from scipy.stats import fisher_exact, binomtest

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")
OUT_CSV      = os.path.join(ANALYSIS_DIR, "results", "alphabet_separation.csv")

# Baselines feeding the §3.5 claims (S1 has the OEIS-hard subset;
# S3 has oeis_base / oeis_core post-dedupe).
S1_CSV = os.path.join(RESULTS_DIR, "baseline_20260511T091836Z.csv")
S3_CSV = os.path.join(RESULTS_DIR, "baseline_20260514T024454Z.csv")

# S3 dedupe rule (drop oeis_base rows whose oeis_xref also appears in S1
# at a different alphabet ⇒ counted once on the smaller-alphabet side).
DROP_XREF = {"A000069", "A000120", "A001969", "A002113"}


def load(csv_path, apply_dedupe=False):
    rows = []
    for r in csv.DictReader(open(csv_path)):
        if apply_dedupe and r.get("category") == "oeis_base" and r.get("oeis_xref") in DROP_XREF:
            continue
        rows.append({
            "id":  r["id"],
            "cat": r["category"],
            "A":   int(r["A"]),
            "cls": r["solomonoff_class"],
        })
    return rows


def rate_table(rows, label, alphabets):
    """Per-alphabet (discovered, n, rate) for the given alphabet list."""
    out = []
    for A in alphabets:
        sub = [r for r in rows if r["A"] == A]
        n = len(sub)
        d = sum(1 for r in sub if r["cls"] == "discovered")
        out.append((label, A, d, n, d / n if n else float("nan")))
    return out


def fisher_2x2(disc_bin, n_bin, disc_lg, n_lg):
    """2x2: rows = {binary, larger}; cols = {discovered, not}. Two-tailed.

    USE WHEN: the binary and larger-alphabet rows are independent samples
    (different sequences in each row). NOT VALID for the §3.5 hardest subset
    because the same 26 OEIS A-numbers are rendered at all three alphabets,
    making the observations paired. Reported only for completeness.
    """
    table = [[disc_bin,            n_bin - disc_bin],
             [disc_lg,             n_lg  - disc_lg]]
    odds, p = fisher_exact(table, alternative="two-sided")
    return table, odds, p


def mcnemar_exact(pairs):
    """McNemar's exact (binomial) two-sided test on paired binary outcomes.

    USE WHEN: each subject contributes two paired measurements (here, the
    same OEIS A-number rendered at two alphabets). Tests whether the rate
    of (0,1) discordant pairs equals the rate of (1,0) discordant pairs
    under H0. This is the CORRECT test for the §3.5 hardest subset.

    Returns (n01, n10, p_two_sided).
    """
    n01 = sum(1 for a, b in pairs if not a and b)      # disc only in B
    n10 = sum(1 for a, b in pairs if a and not b)      # disc only in A
    nd  = n01 + n10
    if nd == 0:
        return n01, n10, 1.0
    # Exact two-sided binomial test on the smaller arm.
    p = binomtest(min(n01, n10), nd, p=0.5, alternative="two-sided").pvalue
    return n01, n10, p


def split_id(seq_id):
    """Parse 'A079315_a2' → ('A079315', 2). Returns (None, None) if not OEIS."""
    m = re.match(r'^(A\d+)_a(\d+)$', seq_id)
    if not m: return None, None
    return m.group(1), int(m.group(2))


def main():
    s1 = load(S1_CSV, apply_dedupe=False)
    s3 = load(S3_CSV, apply_dedupe=True)

    # ----- Claim (1): the 54/33/27 progression — S3 oeis_base ---------
    print("=" * 72)
    print(" Claim (1): rate progression 54% binary → 33% ternary → 27% A=4")
    print(" Source: S3 oeis_base (deduped, 161 sequences per alphabet A=2,3,4)")
    print("=" * 72)
    oeis_base = [r for r in s3 if r["cat"] == "oeis_base"]
    rates_base = rate_table(oeis_base, "S3 oeis_base", [2, 3, 4])
    for label, A, d, n, p in rates_base:
        print(f"  {label}  A={A}: {d}/{n} = {p*100:.1f}%")
    paper_targets = {2: 0.54, 3: 0.33, 4: 0.27}
    print("\n  vs paper text (54% / 33% / 27%):")
    for label, A, d, n, p in rates_base:
        match = abs(p - paper_targets[A]) <= 0.01
        print(f"    A={A}: empirical {p*100:.2f}% vs paper {paper_targets[A]*100:.0f}% "
              f"→ {'CONFIRMED' if match else 'differs'}")

    # ----- Claim (2): the hardest OEIS subset — S1 oeis_hard ----------
    print()
    print("=" * 72)
    print(" Claim (2): hardest OEIS subset — binary ≈ 30%, larger A → 0%")
    print(" Source: S1 oeis_hard (26 sequences per alphabet A=2,3,4)")
    print("=" * 72)
    hard = [r for r in s1 if r["cat"] == "oeis_hard"]
    rates_hard = rate_table(hard, "S1 oeis_hard", [2, 3, 4])
    for label, A, d, n, p in rates_hard:
        print(f"  {label}  A={A}: {d}/{n} = {p*100:.1f}%")

    # 2x2 contingency: binary (A=2) vs larger (A>=3 grouped)
    disc_bin = sum(1 for r in hard if r["A"] == 2 and r["cls"] == "discovered")
    n_bin    = sum(1 for r in hard if r["A"] == 2)
    disc_lg  = sum(1 for r in hard if r["A"] >= 3 and r["cls"] == "discovered")
    n_lg     = sum(1 for r in hard if r["A"] >= 3)

    print(f"\n  2x2 contingency table:")
    print(f"                 discovered  not_discovered  total")
    print(f"    binary (A=2):    {disc_bin:>3d}             {n_bin-disc_bin:>3d}      {n_bin:>3d}")
    print(f"    larger (A≥3):    {disc_lg:>3d}             {n_lg-disc_lg:>3d}      {n_lg:>3d}")
    print(f"    total:           {disc_bin+disc_lg:>3d}             {n_bin-disc_bin+n_lg-disc_lg:>3d}      {n_bin+n_lg:>3d}")

    # --- Check whether the rows at A=2 / A=3 / A=4 are paired ----------
    # Parse oeis_xref from id format 'A079315_a2' and intersect.
    by_A_xrefs = defaultdict(set)
    by_A_xref_outcome = {}
    for r in hard:
        xref, _ = split_id(r["id"])
        if xref is None: continue
        by_A_xrefs[r["A"]].add(xref)
        by_A_xref_outcome[(xref, r["A"])] = (r["cls"] == "discovered")
    shared = sorted(by_A_xrefs[2] & by_A_xrefs[3] & by_A_xrefs[4])
    print(f"\n  Pairing check: |xref(A=2) ∩ xref(A=3) ∩ xref(A=4)| = {len(shared)}")
    paired = (len(shared) == n_bin == 26 and
              len(by_A_xrefs[3]) == 26 and len(by_A_xrefs[4]) == 26)
    print(f"  All three alphabets share the same {len(shared)} OEIS A-numbers: "
          f"{'YES → paired observations' if paired else 'NO → independent samples'}")

    # --- Fisher reported for completeness; not the correct test here ---
    table, odds, p_fisher = fisher_2x2(disc_bin, n_bin, disc_lg, n_lg)
    print(f"\n  [Fisher's exact two-tailed — for completeness only; not valid here]")
    print(f"     odds ratio: {'∞ (zero discoveries in larger A)' if odds == float('inf') else f'{odds:.4g}'}")
    print(f"     exact p:    {p_fisher:.4g}")

    # --- McNemar exact (the correct test) -----------------------------
    pairs_23 = [(by_A_xref_outcome[(x, 2)], by_A_xref_outcome[(x, 3)]) for x in shared]
    pairs_24 = [(by_A_xref_outcome[(x, 2)], by_A_xref_outcome[(x, 4)]) for x in shared]
    pairs_2_vs_lg = [
        (by_A_xref_outcome[(x, 2)],
         by_A_xref_outcome[(x, 3)] or by_A_xref_outcome[(x, 4)])
        for x in shared
    ]
    n01_23, n10_23, p_23 = mcnemar_exact(pairs_23)
    n01_24, n10_24, p_24 = mcnemar_exact(pairs_24)
    n01_lg, n10_lg, p_lg = mcnemar_exact(pairs_2_vs_lg)

    disc_2_only_v3 = [x for x in shared if by_A_xref_outcome[(x, 2)] and not by_A_xref_outcome[(x, 3)]]
    disc_3_only    = [x for x in shared if not by_A_xref_outcome[(x, 2)] and by_A_xref_outcome[(x, 3)]]
    disc_2_only_v4 = [x for x in shared if by_A_xref_outcome[(x, 2)] and not by_A_xref_outcome[(x, 4)]]
    disc_4_only    = [x for x in shared if not by_A_xref_outcome[(x, 2)] and by_A_xref_outcome[(x, 4)]]

    print(f"\n  McNemar's exact (binomial) two-sided — the correct test")
    print(f"  ────────────────────────────────────────────────────────")
    print(f"  Binary vs Ternary (n = {len(shared)} paired sequences)")
    print(f"    disc at A=2 but fail at A=3: {n10_23}  (ids: {disc_2_only_v3})")
    print(f"    fail at A=2 but disc at A=3: {n01_23}  (ids: {disc_3_only})")
    print(f"    total discordant:            {n10_23 + n01_23}")
    print(f"    exact two-sided p:           {p_23:.4f}")
    print(f"  Binary vs Quaternary (n = {len(shared)} paired sequences)")
    print(f"    disc at A=2 but fail at A=4: {n10_24}  (ids: {disc_2_only_v4})")
    print(f"    fail at A=2 but disc at A=4: {n01_24}  (ids: {disc_4_only})")
    print(f"    total discordant:            {n10_24 + n01_24}")
    print(f"    exact two-sided p:           {p_24:.4f}")

    # ----- Save side-car CSV ------------------------------------------
    os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)
    with open(OUT_CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["claim", "label", "alphabet", "discovered", "n", "rate_or_p"])
        for label, A, d, n, rate in rates_base:
            w.writerow(["progression_54_33_27", label, A, d, n, f"{rate:.4f}"])
        for label, A, d, n, rate in rates_hard:
            w.writerow(["hardest_subset", label, A, d, n, f"{rate:.4f}"])
        w.writerow(["fisher_exact_2x2_NOT_VALID", "S1 oeis_hard binary vs A≥3",
                    "test", disc_bin + disc_lg, n_bin + n_lg, f"p={p_fisher:.4g}"])
        w.writerow(["mcnemar_exact_binary_vs_ternary", "S1 oeis_hard (paired)",
                    "test", n10_23 + n01_23, len(shared), f"p={p_23:.4f}"])
        w.writerow(["mcnemar_exact_binary_vs_quaternary", "S1 oeis_hard (paired)",
                    "test", n10_24 + n01_24, len(shared), f"p={p_24:.4f}"])
    print(f"\n  wrote {OUT_CSV}")

    # ----- Verdict ----------------------------------------------------
    print()
    print("=" * 72)
    print(" Verdict for the §3.5 sentence (paired-sample analysis)")
    print("=" * 72)
    print(f"  hardest subset: S1 oeis_hard, {len(shared)} OEIS A-numbers, each")
    print(f"                  measured at A=2, A=3, A=4 (paired observations)")
    print(f"  rates:          A=2 {disc_bin}/26 = {100*disc_bin/26:.1f}%, "
          f"A=3 0/26 = 0%, A=4 0/26 = 0%")
    print(f"  test:           McNemar exact (binomial) two-sided")
    print(f"                  binary vs ternary:    {n10_23 + n01_23} discordant, p = {p_23:.4f}")
    print(f"                  binary vs quaternary: {n10_24 + n01_24} discordant, p = {p_24:.4f}")
    if p_23 < 1e-6 or p_24 < 1e-6:
        print(f"  ✓ 'a separation significant beyond one in a million' holds.")
    else:
        worst = max(p_23, p_24)
        print(f"  ✗ p ≈ {worst:.4f} is NOT below 1e-6; the paper sentence overstates.")
        print(f"     Honest substitute: 'McNemar exact two-sided, "
              f"{n10_23 + n01_23} discordant pairs, p ≈ {p_23:.3f}'.")


if __name__ == "__main__":
    main()
