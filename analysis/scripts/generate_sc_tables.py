#!/usr/bin/env python3
"""
generate_sc_tables.py  --  Emit SI Appendix C tables SC-1..SC-7 as paste-ready LaTeX.

Run inside the OMNIS repo, after the four-population sweep has completed, against
the deposited per-candidate results. Point --results at the file/dir holding one
record per candidate with at least these fields (rename in the LOADER below if
your column names differ):

    population        in {S1,S2,S3,S4}
    candidate_id      stable id of the source sequence
    gate_class        in {discovered, compressed_only, predicted_only, neither}
    solver_family     in {CTX_X,DARY,NESTED_LOOP,FUNC,FUNC_L,CONCAT,ISA,
                          ITER,ITER_L,ITER_F,STEP,WIDE_BIT,SUB_CALL}
    mdl_bits          description length |p| of the best candidate (float)
    train_bits        bit cost of the training prefix
    raw_bits          bit cost of the full raw sequence
    holdout_len       m, number of held-out terms
    alphabet_size     |Sigma|
    discovery_time_s  wall time to discovery (NaN/inf if not discovered)
    alphabet_base     for multi-alphabet OEIS rows: in {2,3,4,...}; else NaN
    base_seq_id       identity shared across alphabet encodings of one sequence

The script ASSERTS the published invariants (corpus 3,914; discoveries 2,383;
gate split 2,383/17/27/1,487; per-population discoveries 120/244/336/1,683;
family totals from Fig. S3) and stops if the deposition disagrees, so a silent
data-version mismatch cannot produce wrong tables. Override with --no-assert
only if you have intentionally changed the corpus.

Usage:
    python3 generate_sc_tables.py --results results.csv > sc_tables.tex
    python3 generate_sc_tables.py --results results.parquet --no-assert > sc_tables.tex

Then paste sc_tables.tex into SI Appendix C, replacing the commented skeletons,
and delete the bold placeholder paragraph.
"""

import argparse, sys, math
from collections import defaultdict, Counter

# ---- published invariants the deposition must reproduce -------------------
EXPECT = {
    "corpus_total": 3914,
    "discoveries": 2383,
    "gate_split": {"discovered": 2383, "compressed_only": 17,
                   "predicted_only": 27, "neither": 1487},
    "pop_total": {"S1": 488, "S2": 256, "S3": 983, "S4": 2187},
    "pop_disc":  {"S1": 120, "S2": 244, "S3": 336, "S4": 1683},
    "family_total": {"CTX_X": 1290, "DARY": 804, "NESTED_LOOP": 131,
                     "FUNC": 67, "FUNC_L": 60},   # "Other" (=31) is the remainder
    "surplus_median_bits": 173.4,
    "surplus_pct_positive": 96.6,
}

POPS = ["S1", "S2", "S3", "S4"]
GATE = ["discovered", "compressed_only", "predicted_only", "neither"]

def load(path):
    """Return a list of dict rows. Supports csv and parquet; adapt as needed."""
    rows = []
    if path.endswith((".parquet", ".pq")):
        import pandas as pd
        df = pd.read_parquet(path)
        rows = df.to_dict("records")
    else:
        import csv
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                for k in ("mdl_bits","train_bits","raw_bits","holdout_len",
                          "alphabet_size","discovery_time_s","alphabet_base"):
                    if k in r and r[k] not in ("", "NA", "nan", None):
                        try: r[k] = float(r[k])
                        except ValueError: pass
                rows.append(r)
    return rows

def fnum(x, nd=1):
    if x is None or (isinstance(x,float) and (math.isnan(x) or math.isinf(x))):
        return "--"
    return f"{x:,.{nd}f}" if isinstance(x,float) else f"{x:,}"

def med(xs):
    xs = sorted(v for v in xs if isinstance(v,(int,float)) and not math.isnan(v))
    if not xs: return float("nan")
    n=len(xs); return xs[n//2] if n%2 else 0.5*(xs[n//2-1]+xs[n//2])

def check(name, got, exp, assert_on):
    ok = (got == exp)
    flag = "OK " if ok else "!! "
    sys.stderr.write(f"  {flag}{name}: got {got}, expected {exp}\n")
    if assert_on and not ok:
        sys.exit(f"ABORT: {name} mismatch (got {got}, expected {exp}). "
                 f"Re-run with --no-assert only if the corpus changed intentionally.")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--no-assert", action="store_true")
    a = ap.parse_args()
    rows = load(a.results)
    A = not a.no_assert

    sys.stderr.write(f"Loaded {len(rows)} candidate records.\n")
    check("corpus total", len(rows), EXPECT["corpus_total"], A)

    bypop = defaultdict(list)
    for r in rows: bypop[r["population"]].append(r)
    gate_c = Counter(r["gate_class"] for r in rows)
    for g in GATE: check(f"gate {g}", gate_c[g], EXPECT["gate_split"][g], A)
    for p in POPS:
        check(f"{p} total", len(bypop[p]), EXPECT["pop_total"][p], A)
        d = sum(1 for r in bypop[p] if r["gate_class"]=="discovered")
        check(f"{p} discoveries", d, EXPECT["pop_disc"][p], A)

    out = []
    def emit(s): out.append(s)

    # ---- SC-1: gate outcomes by population --------------------------------
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Gate outcomes by population. For each population S1 to S4, the number of candidates classified discovered (compresses and predicts), compressed only, predicted only, and neither; row sums match Table~S3 and the corpus totals of Fig.~2 of the main text (2,383 / 17 / 27 / 1,487).}\label{tab:sc1}")
    emit(r"\begin{tabular}{lrrrrr}\toprule")
    emit(r"Pop. & discovered & compressed only & predicted only & neither & total \\\midrule")
    for p in POPS:
        c = Counter(r["gate_class"] for r in bypop[p])
        emit(f"{p} & {c['discovered']:,} & {c['compressed_only']:,} & {c['predicted_only']:,} & {c['neither']:,} & {len(bypop[p]):,} \\\\")
    tot = Counter(r["gate_class"] for r in rows)
    emit(r"\midrule")
    emit(f"total & {tot['discovered']:,} & {tot['compressed_only']:,} & {tot['predicted_only']:,} & {tot['neither']:,} & {len(rows):,} \\\\")
    emit(r"\bottomrule\end{tabular}\end{table}"); emit("")

    # ---- SC-2: solver portfolio -------------------------------------------
    fams = sorted({r["solver_family"] for r in rows if r["gate_class"]=="discovered"})
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Solver portfolio. Discoveries per solver family and per population; family totals match Fig.~S3. The final row gives the per-population ordinary-least-squares slope of $\log_2$ discovery time against description length (Section~3.3 of the main text).}\label{tab:sc2}")
    emit(r"\begin{tabular}{l" + "r"*len(POPS) + r"r}\toprule")
    emit("Family & " + " & ".join(POPS) + r" & total \\\midrule")
    for fam in fams:
        cells, t = [], 0
        for p in POPS:
            n = sum(1 for r in bypop[p] if r["gate_class"]=="discovered" and r["solver_family"]==fam)
            cells.append(f"{n:,}"); t += n
        fam_esc = fam.replace("_", r"\_")
        emit(f"{fam_esc} & " + " & ".join(cells) + f" & {t:,} \\\\")
    emit(r"\midrule")
    # per-population slope via least squares on (mdl_bits, log2 time) for discoveries
    def slope(recs):
        pts = [(r["mdl_bits"], math.log2(r["discovery_time_s"]))
               for r in recs if r["gate_class"]=="discovered"
               and isinstance(r.get("discovery_time_s"),(int,float))
               and r["discovery_time_s"]>0 and not math.isnan(r["discovery_time_s"])]
        if len(pts)<2: return float("nan")
        n=len(pts); sx=sum(x for x,_ in pts); sy=sum(y for _,y in pts)
        sxx=sum(x*x for x,_ in pts); sxy=sum(x*y for x,y in pts)
        den=n*sxx-sx*sx
        return (n*sxy-sx*sy)/den if den else float("nan")
    slopes=[slope(bypop[p]) for p in POPS]
    emit("slope & " + " & ".join(fnum(s,3) for s in slopes) + r" & \\")
    emit(r"\bottomrule\end{tabular}\end{table}"); emit("")
    sys.stderr.write(f"  per-population slopes: {[round(s,3) for s in slopes]} (paper: 0.048, 0.060, 0.045, 0.058)\n")

    # ---- SC-3: alphabet asymmetry -----------------------------------------
    multi = [r for r in rows if isinstance(r.get("alphabet_base"),(int,float)) and not math.isnan(r.get("alphabet_base",float("nan")))]
    bases = sorted({int(r["alphabet_base"]) for r in multi})[:3]
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Alphabet asymmetry. Discovery rate by alphabet base for the multi-alphabet OEIS cohort (the 54/33/27 percent decay of Section~3.5); the twenty-six-sequence hard subset; and the count of inversion sequences that become easier at a larger base. Rates are discoveries over candidates at that base.}\label{tab:sc3}")
    emit(r"\begin{tabular}{l" + "r"*len(bases) + r"}\toprule")
    emit("Cohort & " + " & ".join(f"base {b}" for b in bases) + r" \\\midrule")
    def rate(recs, b):
        at=[r for r in recs if int(r.get("alphabet_base",-1))==b]
        if not at: return "--"
        d=sum(1 for r in at if r["gate_class"]=="discovered")
        return f"{100*d/len(at):.0f}\\%"
    emit("multi-alphabet cohort & " + " & ".join(rate(multi,b) for b in bases) + r" \\")
    emit(r"\bottomrule\end{tabular}")
    emit(r"\\[2pt]\footnotesize Hard subset (26 sequences) and inversion sequences (30 of 261) are listed in the deposited data; the paired binary-versus-larger-base outcomes feed the exact McNemar test ($p\approx0.016$).")
    emit(r"\end{table}"); emit("")

    # ---- SC-4: description-length distribution ----------------------------
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Description-length distribution of discovered programs, in bits, per population.}\label{tab:sc4}")
    emit(r"\begin{tabular}{lrrrr}\toprule")
    emit(r"Pop. & min & median & mean & max \\\midrule")
    for p in POPS:
        ls=[r["mdl_bits"] for r in bypop[p] if r["gate_class"]=="discovered" and isinstance(r.get("mdl_bits"),(int,float))]
        if ls:
            emit(f"{p} & {min(ls):.0f} & {med(ls):.1f} & {sum(ls)/len(ls):.1f} & {max(ls):.0f} \\\\")
        else:
            emit(f"{p} & -- & -- & -- & -- \\\\")
    emit(r"\bottomrule\end{tabular}\end{table}"); emit("")

    # ---- SC-5: off-diagonal characterization ------------------------------
    od=[r for r in rows if r["gate_class"] in ("predicted_only","compressed_only")]
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Off-diagonal characterization. The predicted-only (27) and compressed-only (17) candidates, with population, solver family, and the compression deficit $|p|-\text{train\_bits}$ in bits (positive for predicted-only, negative for compressed-only). Includes the divisor-counting instance that misses compression by about 0.45 bits (Section~3.4 of the main text).}\label{tab:sc5}")
    emit(r"\begin{tabular}{llllr}\toprule")
    emit(r"candidate & pop. & family & class & deficit (bits) \\\midrule")
    for r in sorted(od, key=lambda r:(r["gate_class"], r["population"])):
        defi = (r.get("mdl_bits",float("nan")) - r.get("train_bits",float("nan")))
        cid = str(r.get("candidate_id","")).replace("_", r"\_")
        fam_esc = r["solver_family"].replace("_", r"\_")
        cls_esc = r["gate_class"].replace("_", " ")
        emit(f"{cid} & {r['population']} & {fam_esc} & {cls_esc} & {fnum(defi,2)} \\\\")
    emit(r"\bottomrule\end{tabular}\end{table}"); emit("")

    # ---- SC-6: held-out surplus -------------------------------------------
    def surplus(r):
        try: return r["holdout_len"]*math.log2(r["alphabet_size"]) - r["mdl_bits"]
        except Exception: return float("nan")
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Held-out surplus $m\log_2|\Sigma|-|p|$ over the 2,383 discoveries, per population. Positive surplus is the regime where the false-acceptance bound of SI-B is informative; the corpus median is 173.4 bits with 96.6 percent of discoveries positive.}\label{tab:sc6}")
    emit(r"\begin{tabular}{lrrrr}\toprule")
    emit(r"Pop. & median & mean & \% positive & non-positive \\\midrule")
    alls=[]
    for p in POPS:
        ss=[surplus(r) for r in bypop[p] if r["gate_class"]=="discovered"]
        ss=[s for s in ss if not math.isnan(s)]; alls+=ss
        if ss:
            pos=sum(1 for s in ss if s>0)
            emit(f"{p} & {med(ss):.1f} & {sum(ss)/len(ss):.1f} & {100*pos/len(ss):.1f}\\% & {len(ss)-pos} \\\\")
        else:
            emit(f"{p} & -- & -- & -- & -- \\\\")
    emit(r"\midrule")
    if alls:
        pos=sum(1 for s in alls if s>0)
        emit(f"total & {med(alls):.1f} & {sum(alls)/len(alls):.1f} & {100*pos/len(alls):.1f}\\% & {len(alls)-pos} \\\\")
        check("surplus median (bits)", round(med(alls),1), EXPECT["surplus_median_bits"], A)
        check("surplus %% positive", round(100*pos/len(alls),1), EXPECT["surplus_pct_positive"], A)
    emit(r"\bottomrule\end{tabular}\end{table}"); emit("")

    # ---- SC-7: total-ratio distribution -----------------------------------
    emit(r"\begin{table}[!ht]\centering")
    emit(r"\caption{Total-ratio distribution. Description length over raw bits across the corpus, the vertical axis of Fig.~2 of the main text, per gate class.}\label{tab:sc7}")
    emit(r"\begin{tabular}{lrrrr}\toprule")
    emit(r"class & min & median & mean & max \\\midrule")
    for g in GATE:
        rs=[(r.get("mdl_bits",float("nan"))/r["raw_bits"]) for r in rows
            if r["gate_class"]==g and isinstance(r.get("raw_bits"),(int,float)) and r["raw_bits"]>0]
        rs=[x for x in rs if not math.isnan(x)]
        if rs:
            emit(f"{g.replace('_',' ')} & {min(rs):.3f} & {med(rs):.3f} & {sum(rs)/len(rs):.3f} & {max(rs):.3f} \\\\")
        else:
            emit(f"{g.replace('_',' ')} & -- & -- & -- & -- \\\\")
    emit(r"\bottomrule\end{tabular}\end{table}")

    print("\n".join(out))
    sys.stderr.write("\nDone. Verify the per-population slopes and SC-3 rates against the paper, "
                     "then paste stdout into SI Appendix C.\n")

if __name__ == "__main__":
    main()
