#!/usr/bin/env python3
"""
analyze_omnis.py  --  validation analyses for "The Program Is Still There".

It computes ONLY from data you provide. It never assumes an outcome, and it
prints each result next to the exact \toinsert{...} label it fills in
omnis_validation_addenda.tex.

Three analyses:
  (1) Per-instance scaling regression: log2(time) ~ program_length_bits,
      pooled + per-population, OLS + Spearman + Theil-Sen, with CI/R^2/p/n.
  (2) Gate false-acceptance surplus: m*log2(|Sigma|) - L per discovery, and the
      implied per-instance bound 2^-(surplus-1)  (Proposition: gate-fap).
  (3) Null tabulation: discovery rate on shuffled / random sequences, with an
      exact (Clopper-Pearson) upper confidence bound.

--------------------------------------------------------------------------
EXPECTED INPUT (CSV). Column names are configurable via the constants below.

  discoveries.csv  (one row per DISCOVERY, i.e. both gate conditions passed)
    required for (1): time_s, program_bits, population
    required for (2): holdout_len, alphabet_size, program_bits, population
    optional        : kappa, nesting_depth   (extra regressors for (1))

  null.csv  (one row per NULL TRIAL on a shuffled/random sequence)
    required for (3): null_type in {shuffled, random}, population,
                      compressed (0/1), predicted (0/1)
    a row is a "discovery" iff compressed==1 AND predicted==1
--------------------------------------------------------------------------

USAGE
  pip install pandas numpy scipy matplotlib
  python3 analyze_omnis.py --discoveries discoveries.csv --null null.csv \
      --outdir results/

If --null is omitted, analysis (3) is skipped. If columns for (2) are absent,
(2) is skipped. Nothing is fabricated; missing inputs produce a clear notice.
"""

import argparse, math, sys, os

# ---- configurable column names ------------------------------------------
COL = dict(
    time="time_s",
    bits="program_bits",
    pop="population",
    kappa="kappa",
    depth="nesting_depth",
    holdout="holdout_len",
    alphabet="alphabet_size",
    null_type="null_type",
    compressed="compressed",
    predicted="predicted",
)


def need(msg):
    print(f"  [skipped] {msg}")


def clopper_pearson_upper(k, n, alpha=0.05):
    """Exact upper 1-alpha confidence bound on a binomial rate (k of n)."""
    if n == 0:
        return float("nan")
    if k == n:
        return 1.0
    from scipy.stats import beta
    return float(beta.ppf(1 - alpha, k + 1, n - k))


def _ols(x, y, stats):
    """OLS y~x via scipy. Returns (slope, ci_lo, ci_hi, r2, p, n)."""
    n = len(x)
    lr = stats.linregress(x, y)
    se = lr.stderr
    tcrit = stats.t.ppf(0.975, max(n - 2, 1))
    return lr.slope, lr.slope - tcrit * se, lr.slope + tcrit * se, lr.rvalue ** 2, lr.pvalue, n


def regression(df, np, stats):
    print("\n=== (1) Per-instance scaling regression ============================")
    c = COL
    miss = [c[x] for x in ("time", "bits", "pop") if c[x] not in df.columns]
    if miss:
        need(f"need columns {miss} in discoveries.csv for the regression")
        return
    d = df[[c["time"], c["bits"], c["pop"]]].copy()
    d = d[(d[c["time"]] > 0) & (d[c["bits"]] > 0)].dropna()
    n = len(d)
    if n < 10:
        need(f"only {n} usable discovery rows; need >= 10")
        return
    y = np.log2(d[c["time"]].to_numpy(dtype=float))
    x = d[c["bits"]].to_numpy(dtype=float)

    slope, ci_lo, ci_hi, r2, pval, _ = _ols(x, y, stats)
    rho, rho_p = stats.spearmanr(x, y)
    ts = stats.theilslopes(y, x, 0.95)  # slope, intercept, lo, hi

    print(f"  n discoveries (usable)      : {n}")
    print(f"  OLS slope (log2 time / bit) : {slope:.4f}   [toinsert: slope]")
    print(f"  OLS slope 95% CI            : [{ci_lo:.4f}, {ci_hi:.4f}]   [toinsert: slope CI]")
    print(f"  R^2                         : {r2:.4f}   [toinsert: R2]")
    print(f"  p (slope != 0)              : {pval:.3e}   [toinsert: p value]")
    print(f"  Theil-Sen slope (robust)    : {ts[0]:.4f}  CI [{ts[2]:.4f}, {ts[3]:.4f}]   [toinsert: theilsen slope]")
    print(f"  Spearman rho (per-instance) : {rho:.4f}  (p={rho_p:.3e})   [toinsert: spearman]")
    print(f"  PREDICTED slope band        : 0.94 (d=8) to 0.99 (d=53), up to per-candidate constant")
    verdict = ("consistent with" if ci_lo <= 0.99 and ci_hi >= 0.94
               else ("above" if ci_lo > 0.99 else "below"))
    print(f"  -> measured slope is {verdict.upper()} the predicted band   [toinsert: one of consistent/above/below]")

    # per-population fixed-effect slopes
    print("  per-population OLS slopes      [toinsert: per-population slopes]:")
    for p, sub in d.groupby(c["pop"]):
        if len(sub) >= 10 and sub[c["bits"]].nunique() >= 3:
            yy = np.log2(sub[c["time"]].to_numpy(float))
            xx = sub[c["bits"]].to_numpy(float)
            s2, lo2, hi2, r22, _, n2 = _ols(xx, yy, stats)
            print(f"     {p:<28} slope={s2:.4f}  R^2={r22:.3f}  n={n2}")
        else:
            print(f"     {p:<28} (too few/var-poor: n={len(sub)})")

    # optional extra regressors
    for key, label in (("kappa", "coupling width kappa"), ("depth", "nesting depth")):
        if c[key] in df.columns:
            dd = df[[c["time"], c[key]]].copy()
            dd = dd[(dd[c["time"]] > 0) & dd[c[key]].notna()]
            if len(dd) >= 10 and dd[c[key]].nunique() >= 3:
                yy = np.log2(dd[c["time"]].to_numpy(float))
                xx = dd[c[key]].to_numpy(float)
                s3, lo3, hi3, r23, p3, n3 = _ols(xx, yy, stats)
                print(f"  regression on {label}: slope={s3:.4f} "
                      f"CI[{lo3:.4f},{hi3:.4f}] R^2={r23:.3f} p={p3:.2e} n={n3}")

    return d, y, x


def scaling_figure(d, y, x, np, plt, outdir):
    try:
        c = COL
        fig, ax = plt.subplots(figsize=(5.2, 4.0))
        pops = list(d[c["pop"]].unique())
        for p in pops:
            m = (d[c["pop"]] == p).to_numpy()
            ax.scatter(x[m], y[m], s=10, alpha=0.5, label=str(p))
        # pooled fit line
        b1, b0 = np.polyfit(x, y, 1)
        xs = np.linspace(x.min(), x.max(), 100)
        ax.plot(xs, b0 + b1 * xs, "k-", lw=1.5, label=f"fit (slope {b1:.2f})")
        ax.set_xlabel("program description length (bits)")
        ax.set_ylabel(r"$\log_2$ discovery time (s)")
        ax.legend(fontsize=7, loc="best")
        fig.tight_layout()
        path = os.path.join(outdir, "fig_scaling_cost_vs_length.pdf")
        fig.savefig(path)
        print(f"  figure written: {path}")
    except Exception as e:
        need(f"figure not produced ({e})")


def fap_surplus(df, np, plt, outdir):
    print("\n=== (2) Gate false-acceptance surplus (Proposition gate-fap) ========")
    c = COL
    miss = [c[x] for x in ("holdout", "alphabet", "bits") if c[x] not in df.columns]
    if miss:
        need(f"need columns {miss} for the false-acceptance surplus")
        return
    d = df[[c["holdout"], c["alphabet"], c["bits"]]].dropna()
    d = d[(d[c["holdout"]] > 0) & (d[c["alphabet"]] > 1)]
    if len(d) == 0:
        need("no usable rows for the surplus")
        return
    m = d[c["holdout"]].to_numpy(float)
    sig = d[c["alphabet"]].to_numpy(float)
    L = d[c["bits"]].to_numpy(float)
    surplus = m * np.log2(sig) - L  # bits the holdout carries beyond program length
    fap_log2 = -(surplus - 1.0)     # log2 of the per-instance bound 2^-(surplus-1)
    print(f"  discoveries scored          : {len(d)}")
    print(f"  surplus  m*log2|S| - L (bits): min {surplus.min():.1f}, "
          f"median {np.median(surplus):.1f}, max {surplus.max():.1f}")
    frac_pos = float((surplus > 0).mean())
    print(f"  fraction with positive surplus (sound): {frac_pos:.4f}")
    print(f"  median per-instance FAP bound: 2^({np.median(fap_log2):.1f})   [toinsert: median FAP]")
    worst = float(fap_log2.max())  # least-negative exponent = largest bound
    print(f"  worst-case (largest) FAP bound: 2^({worst:.1f})   [toinsert: worst FAP]")
    try:
        fig, ax = plt.subplots(figsize=(5.2, 3.6))
        ax.hist(surplus, bins=40)
        ax.axvline(0, color="r", lw=1)
        ax.set_xlabel(r"held-out surplus  $m\log_2|\Sigma| - |p|$  (bits)")
        ax.set_ylabel("discoveries")
        fig.tight_layout()
        path = os.path.join(outdir, "fig_gate_surplus.pdf")
        fig.savefig(path)
        print(f"  figure written: {path}")
    except Exception as e:
        need(f"surplus figure not produced ({e})")


def null_table(ndf):
    print("\n=== (3) Null tabulation (shuffled / random) ========================")
    c = COL
    miss = [c[x] for x in ("null_type", "compressed", "predicted") if c[x] not in ndf.columns]
    if miss:
        need(f"need columns {miss} in null.csv")
        return
    ndf = ndf.copy()
    disc = (ndf[c["compressed"]].astype(int) == 1) & (ndf[c["predicted"]].astype(int) == 1)
    ndf["_disc"] = disc.astype(int)
    for nt, sub in ndf.groupby(c["null_type"]):
        n = len(sub)
        k = int(sub["_disc"].sum())
        kc = int((sub[c["compressed"]].astype(int) == 1).sum())
        kp = int((sub[c["predicted"]].astype(int) == 1).sum())
        up = clopper_pearson_upper(k, n)
        print(f"  [{nt}] trials={n}  discoveries={k}  rate={k/n if n else float('nan'):.5f}  "
              f"95% upper={up:.5f}   [toinsert: {nt} discoveries / {nt} rate / {nt} CP upper]")
        print(f"        compress-only count={kc} ({kc/n if n else 0:.4f}), "
              f"predict-only count={kp} ({kp/n if n else 0:.4f})")
    print("  (compare to structured corpus: 2383/3914 = 0.609)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--discoveries", required=True)
    ap.add_argument("--null", default=None)
    ap.add_argument("--outdir", default="results")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    try:
        import numpy as np, pandas as pd
        from scipy import stats
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"missing dependency: {e}\n  pip install pandas numpy scipy matplotlib")
        sys.exit(1)

    df = pd.read_csv(args.discoveries)
    print(f"loaded {len(df)} discovery rows from {args.discoveries}; columns: {list(df.columns)}")

    out = regression(df, np, stats)
    if out:
        d, y, x = out
        scaling_figure(d, y, x, np, plt, args.outdir)
    fap_surplus(df, np, plt, args.outdir)

    if args.null:
        ndf = pd.read_csv(args.null)
        print(f"\nloaded {len(ndf)} null rows from {args.null}")
        null_table(ndf)
    else:
        print("\n=== (3) Null tabulation ============================================")
        need("no --null CSV given; run the engine on shuffled/random sequences "
             "(Protocol 1A) and pass the outcomes here")

    print("\nDone. Paste the printed numbers into the matching \\toinsert{...} "
          "fields of omnis_validation_addenda.tex.")


if __name__ == "__main__":
    main()
