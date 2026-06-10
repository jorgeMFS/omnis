#!/usr/bin/env python3
"""
null_tiers.py -- reproduce the entropy-stratified null table (tab:snull) and the
H>=0.7 false-alarm headline from the raw sweep outputs. Discoveries are counted
by the corpus criterion (solomonoff_class == "discovered"), identical to
build_discoveries_csv.py and the fixed build_null_csv.py. Read-only; fabricates
nothing.
"""
import csv, os, sys
from scipy.stats import beta  # same dependency analyze_omnis.py uses

ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SWEEP_DIR    = os.path.join(ANALYSIS_DIR, "null_sweep")
RAW    = os.path.join(SWEEP_DIR, "null_raw.csv")
SAMPLE = os.path.join(SWEEP_DIR, "null_sample.csv")
AUDIT  = os.path.join(SWEEP_DIR, "null_workload_audit.csv")
OUT    = os.path.join(ANALYSIS_DIR, "results", "null_tiers.csv")

def cp_upper(k, n, alpha=0.05):           # one-sided 95% upper
    if n == 0: return float("nan")
    if k >= n: return 1.0
    return beta.ppf(1 - alpha, k + 1, n - k)

def tier(h):
    if h >= 0.7: return "high"
    if h >= 0.5: return "moderate"
    if h >= 0.1: return "low"
    return "near-const"

def main():
    for p in (RAW, SAMPLE, AUDIT):
        if not os.path.isfile(p): sys.exit(f"missing {p}")
    pop = {r["original_id"]: r["population"]        for r in csv.DictReader(open(SAMPLE))}
    H   = {r["original_id"]: float(r["multiset_H"]) for r in csv.DictReader(open(SAMPLE))}
    backmap = {r["shuffled_id"]: r["original_id"]   for r in csv.DictReader(open(AUDIT))}

    rows = []
    for r in csv.DictReader(open(RAW)):
        orig = backmap.get(r["id"])
        if not orig: sys.exit(f"id {r['id']} not in workload audit")
        sclass = r["solomonoff_class"]
        rows.append({
            "pop": pop[orig], "H": H[orig],
            "disc": 1 if sclass == "discovered" else 0,
            "po":   1 if sclass == "not_compressed_predicted" else 0,
            "tier": tier(H[orig]),
        })

    order = ["high", "moderate", "low", "near-const"]
    print("=== tab:snull (entropy-stratified) ===")
    print(f"{'tier':12s} {'n':>4s} {'disc':>5s} {'pred_only':>9s} {'CP95up':>8s}")
    table = []
    for t in order:
        s = [x for x in rows if x["tier"] == t]
        n = len(s); k = sum(x["disc"] for x in s); po = sum(x["po"] for x in s)
        cp = cp_upper(k, n)
        print(f"{t:12s} {n:4d} {k:5d} {po:9d} {cp*100:7.2f}%")
        table.append((t, n, k, po, cp))

    hi = [x for x in rows if x["tier"] == "high"]
    n = len(hi); k = sum(x["disc"] for x in hi)
    print("\n=== H>=0.7 false-alarm headline ===")
    print(f"[toinsert: shuffled discoveries] = {k}")
    print(f"[toinsert: shuffled rate]        = {k/n:.4f}")
    print(f"[toinsert: shuffled CP upper]    = {cp_upper(k,n):.4f} ({cp_upper(k,n)*100:.2f}%)")
    print("per-population within H>=0.7:")
    for p in ("S1", "S2", "S3", "S4"):
        sp = [x for x in hi if x["pop"] == p]
        np_ = len(sp); kp = sum(x["disc"] for x in sp)
        print(f"  {p}: {kp} / {np_}  CP95up {cp_upper(kp,np_)*100:.2f}%")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["tier","n","discoveries","predict_only","cp95_upper"])
        for t, n_, k_, po_, cp_ in table: w.writerow([t, n_, k_, po_, f"{cp_:.6f}"])
    print(f"\nwrote {OUT}")

if __name__ == "__main__":
    main()
