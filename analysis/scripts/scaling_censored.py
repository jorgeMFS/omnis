#!/usr/bin/env python3
"""
scaling_censored.py - Task 3 of the §3.3 / Fig. S3 review response.

Two analyses:

(1) Censored fit. The OLS slope of log2(time) on program_bits (0.0539 pooled,
    0.0566 deadline-free) treats every observed discovery time as exact, but
    the per-phase deadlines and the 600 s wall right-censor the slowest
    discoveries. Refit with a censored-data likelihood (here a normal Tobit-
    style right-censored regression at the 600 s wall, plus a separate AFT
    log-normal fit). Report the corrected slope and 95% CI; compare with the
    OLS values.

(2) Model comparison on the deadline-free subset (no-CTX_X, n = 1,093).
    Fit (a) exponential log2(time) = a + b·|p|,
        (b) power-law  log2(time) = c + d·log2(|p|),
        (c) low-degree polynomial in |p|.
    Report AIC/BIC, 5-fold-CV RMSE, and which model the data prefers over the
    observed bit range (~10–120 bits).

Read-only on baselines; writes a single side-car to
analysis/results/scaling_censored.csv. Deps: scipy, statsmodels, lifelines.
"""

import csv
import os
import sys
import warnings
warnings.filterwarnings("ignore")

import numpy as np
import pandas as pd
from scipy import stats
from sklearn.model_selection import KFold

ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RESULTS_DIR  = os.path.join(REPO_ROOT, "data", "results")
OUT_CSV      = os.path.join(ANALYSIS_DIR, "results", "scaling_censored.csv")

DROP_XREF = {"A000069","A000120","A001969","A002113"}

# Censoring strategy. The 600 s wall does not censor any observed discoveries
# (the slowest reaches 586.9 s by coincidence; would-be-slower discoveries
# became 'neither' and are absent from the discovered subset). The right
# censoring that matters is at the per-phase deadlines, where families
# visibly cluster (CTX_X at ~42 s, NESTED_LOOP at ~200 s). We use two
# complementary schemes:
#
#   (A) GLOBAL: censor any discovery with time_s ≥ 500 s (very tail-only).
#   (B) PER-FAMILY: censor at each family's p90 - the natural deadline
#       proxy. This is the more honest deadline-aware censoring.
GLOBAL_CENSOR_S    = 500.0
PER_FAMILY_PERCENTILE = 90.0


# ───────────────────────────────────────────────────────────────────────
# Load discoveries with solver_family
# ───────────────────────────────────────────────────────────────────────
def load_discoveries():
    baselines = [
        ("S1","baseline_20260511T091836Z.csv", False),
        ("S2","baseline_20260513T232442Z.csv", False),
        ("S3","baseline_20260514T024454Z.csv", True),
        ("S4","baseline_20260520T155701Z.csv", False),
    ]
    rows = []
    for pop, fn, dedupe in baselines:
        for r in csv.DictReader(open(os.path.join(RESULTS_DIR, fn))):
            if dedupe and r.get("category") == "oeis_base" and r.get("oeis_xref") in DROP_XREF:
                continue
            if r["solomonoff_class"] != "discovered":
                continue
            desc = r["solver_desc"]
            fam = desc.lstrip('"').split(" ", 1)[0] if desc else ""
            t = float(r["time_s"])
            bits = float(r["mdl"])
            if t > 0 and bits > 0:
                rows.append({
                    "pop": pop, "family": fam, "time_s": t, "bits": bits,
                    "log2t": np.log2(t),
                })
    return pd.DataFrame(rows)


# ───────────────────────────────────────────────────────────────────────
# Censored regression (Tobit right-censored normal)
# ───────────────────────────────────────────────────────────────────────
def tobit_right_censored(y, X, censored):
    """Right-censored normal regression by MLE.

    y          : observed response (log2 time).
    X          : design matrix (with intercept column).
    censored   : bool array. True = right-censored at y; False = observed exactly.

    Returns dict with beta, se, ci_lo, ci_hi for each coefficient + sigma + n.
    """
    from scipy.optimize import minimize
    from scipy.stats import norm

    n, p = X.shape
    censored = np.asarray(censored, dtype=bool)
    obs = ~censored

    def negloglik(params):
        beta = params[:p]
        log_sigma = params[p]
        sigma = np.exp(log_sigma)
        mu = X @ beta
        # Observed: gaussian density
        if obs.any():
            ll_obs = norm.logpdf(y[obs], loc=mu[obs], scale=sigma).sum()
        else:
            ll_obs = 0.0
        # Right-censored: P(Y > y) = 1 - Phi((y-mu)/sigma)
        if censored.any():
            z = (y[censored] - mu[censored]) / sigma
            ll_cens = norm.logsf(z).sum()
        else:
            ll_cens = 0.0
        return -(ll_obs + ll_cens)

    # Start from OLS on the observed subset
    beta0, *_ = np.linalg.lstsq(X[obs], y[obs], rcond=None)
    sigma0 = np.std(y[obs] - X[obs] @ beta0)
    x0 = np.concatenate([beta0, [np.log(max(sigma0, 1e-3))]])
    res = minimize(negloglik, x0, method="BFGS")
    beta_hat = res.x[:p]
    sigma_hat = np.exp(res.x[p])
    # Hessian-based SEs
    hess = res.hess_inv if hasattr(res, "hess_inv") else None
    se = np.sqrt(np.diag(hess))[:p] if hess is not None else np.full(p, np.nan)
    z = stats.norm.ppf(0.975)
    return {
        "beta":   beta_hat,
        "se":     se,
        "ci_lo":  beta_hat - z * se,
        "ci_hi":  beta_hat + z * se,
        "sigma":  sigma_hat,
        "n":      n,
        "n_cens": int(censored.sum()),
        "logL":   -res.fun,
    }


# ───────────────────────────────────────────────────────────────────────
# AFT log-normal fit via lifelines (alternative censored estimator)
# ───────────────────────────────────────────────────────────────────────
def aft_lognormal(df, censored_col):
    """Log-normal AFT model on time_s ~ bits, with right-censoring."""
    from lifelines import LogNormalAFTFitter
    sub = df.copy()
    sub["event"] = (~sub[censored_col]).astype(int)
    fitter = LogNormalAFTFitter()
    fitter.fit(sub[["time_s", "event", "bits"]],
               duration_col="time_s", event_col="event")
    # The AFT in log-normal parameterisation:
    # log(T) = X*beta + sigma*epsilon, epsilon ~ N(0,1).
    # The slope on bits gives the change in log(time) per bit; convert to log2.
    beta_ln  = fitter.params_["mu_", "bits"]
    se_ln    = fitter.standard_errors_["mu_", "bits"]
    ci_ln_lo = fitter.confidence_intervals_.loc[("mu_", "bits"), "95% lower-bound"]
    ci_ln_hi = fitter.confidence_intervals_.loc[("mu_", "bits"), "95% upper-bound"]
    return {
        "beta_log2":   beta_ln / np.log(2),
        "ci_log2_lo":  ci_ln_lo / np.log(2),
        "ci_log2_hi":  ci_ln_hi / np.log(2),
        "n":           len(sub),
        "n_cens":      int(sub[censored_col].sum()),
        "logL":        fitter.log_likelihood_,
    }


# ───────────────────────────────────────────────────────────────────────
# Model comparison: exponential vs power-law vs polynomial
# ───────────────────────────────────────────────────────────────────────
def fit_and_score(X, y, k_params, label):
    """OLS fit; return (slope-info, AIC, BIC, RMSE on training, RMSE 5-fold CV)."""
    import statsmodels.api as sm
    res = sm.OLS(y, X).fit()
    rmse_train = float(np.sqrt(np.mean(res.resid ** 2)))
    # 5-fold CV
    kf = KFold(n_splits=5, shuffle=True, random_state=0)
    cv_rmse = []
    for train_idx, test_idx in kf.split(X):
        fit = sm.OLS(y[train_idx], X[train_idx]).fit()
        pred = fit.predict(X[test_idx])
        cv_rmse.append(np.sqrt(np.mean((pred - y[test_idx]) ** 2)))
    return {
        "label":  label,
        "n":      len(y),
        "k":      k_params,
        "AIC":    res.aic,
        "BIC":    res.bic,
        "R2":     res.rsquared,
        "rmse_train": rmse_train,
        "rmse_cv":    float(np.mean(cv_rmse)),
        "summary":    res,
    }


# ───────────────────────────────────────────────────────────────────────
# Main
# ───────────────────────────────────────────────────────────────────────
def main():
    df = load_discoveries()
    n_total = len(df)
    print(f"Loaded {n_total} discoveries with solver_family + bits + time_s.\n")

    # OLS baselines (reproduce the manuscript)
    print("=" * 72)
    print(" (1) Censored fit - refit log2(time) ~ bits on all discoveries")
    print("=" * 72)
    print(f"  Max observed discovery time: {df.time_s.max():.1f} s (no discovery hit the 600 s wall)")
    print(f"  Censoring scheme (A) GLOBAL:     time_s ≥ {GLOBAL_CENSOR_S:.0f} s")
    print(f"  Censoring scheme (B) PER-FAMILY: time_s ≥ family p{PER_FAMILY_PERCENTILE:.0f}")

    # Build the two censoring flags
    df["censored_global"] = df["time_s"] >= GLOBAL_CENSOR_S
    df["censored_per_family"] = False
    family_p90 = {}
    for fam in df["family"].unique():
        thr = np.percentile(df.loc[df["family"] == fam, "time_s"], PER_FAMILY_PERCENTILE)
        family_p90[fam] = thr
        df.loc[df["family"] == fam, "censored_per_family"] = (
            df.loc[df["family"] == fam, "time_s"] >= thr
        )

    print(f"\n  Per-family p90 deadlines (used as scheme-B censoring threshold):")
    for fam, thr in sorted(family_p90.items(), key=lambda kv: -df[df.family==kv[0]].shape[0])[:8]:
        n_fam = int((df["family"] == fam).sum())
        n_c   = int(((df["family"] == fam) & df["censored_per_family"]).sum())
        print(f"     {fam:14s} n={n_fam:>4d}  p90 = {thr:7.1f}s  →  censoring {n_c} ({100*n_c/n_fam:.0f}%)")

    n_cens_global = int(df["censored_global"].sum())
    n_cens_pf     = int(df["censored_per_family"].sum())
    print(f"\n  Total censored - scheme A: {n_cens_global} ({100*n_cens_global/n_total:.1f}%)"
          f"   scheme B: {n_cens_pf} ({100*n_cens_pf/n_total:.1f}%)")

    # OLS pooled (baseline)
    import statsmodels.api as sm
    X = sm.add_constant(df["bits"].values)
    y = df["log2t"].values
    ols_pooled = sm.OLS(y, X).fit()
    ols_slope = ols_pooled.params[1]
    ols_ci    = ols_pooled.conf_int(0.05)[1]
    print(f"\n  OLS (pooled, ignoring censoring):")
    print(f"    slope = {ols_slope:.4f}   95% CI [{ols_ci[0]:.4f}, {ols_ci[1]:.4f}]   R² = {ols_pooled.rsquared:.4f}   n = {n_total}")

    # Censored fits via lifelines AFT log-normal (proper CIs via Fisher information).
    try:
        aft_a = aft_lognormal(df, "censored_global")
        print(f"\n  AFT log-normal - scheme A (censor at {GLOBAL_CENSOR_S:.0f} s tail):")
        print(f"    slope = {aft_a['beta_log2']:.4f}   95% CI [{aft_a['ci_log2_lo']:.4f}, {aft_a['ci_log2_hi']:.4f}]"
              f"   n = {aft_a['n']}, n_cens = {aft_a['n_cens']}")
    except Exception as e:
        aft_a = None
        print(f"  AFT scheme A skipped: {e}")

    try:
        aft_b = aft_lognormal(df, "censored_per_family")
        print(f"\n  AFT log-normal - scheme B (censor at per-family p{PER_FAMILY_PERCENTILE:.0f}):")
        print(f"    slope = {aft_b['beta_log2']:.4f}   95% CI [{aft_b['ci_log2_lo']:.4f}, {aft_b['ci_log2_hi']:.4f}]"
              f"   n = {aft_b['n']}, n_cens = {aft_b['n_cens']}")
    except Exception as e:
        aft_b = None
        print(f"  AFT scheme B skipped: {e}")

    # Tobit with per-family censoring (sanity-check the AFT result)
    tobit_b = tobit_right_censored(y, X, df["censored_per_family"].values)
    print(f"\n  Tobit right-censored normal - scheme B (sanity check):")
    print(f"    slope = {tobit_b['beta'][1]:.4f}   95% CI [{tobit_b['ci_lo'][1]:.4f}, {tobit_b['ci_hi'][1]:.4f}]"
          f"   n_cens = {tobit_b['n_cens']}")

    # Sanity: no-CTX_X subset (deadline-free)
    df_no_ctx = df[df["family"] != "CTX_X"].reset_index(drop=True)
    n_nc = len(df_no_ctx)
    X_nc = sm.add_constant(df_no_ctx["bits"].values)
    y_nc = df_no_ctx["log2t"].values
    ols_nc = sm.OLS(y_nc, X_nc).fit()
    ols_nc_slope = ols_nc.params[1]
    ols_nc_ci    = ols_nc.conf_int(0.05)[1]
    print(f"\n  OLS deadline-free subset (no CTX_X):")
    print(f"    slope = {ols_nc_slope:.4f}   95% CI [{ols_nc_ci[0]:.4f}, {ols_nc_ci[1]:.4f}]   R² = {ols_nc.rsquared:.4f}   n = {n_nc}")

    # Verdict on (a)
    slopes = {
        "OLS pooled":     ols_slope,
        "OLS no-CTX_X":   ols_nc_slope,
    }
    if aft_a is not None: slopes["AFT scheme A"] = aft_a["beta_log2"]
    if aft_b is not None: slopes["AFT scheme B"] = aft_b["beta_log2"]
    slopes["Tobit scheme B"] = tobit_b["beta"][1]

    print()
    print("  ── Verdict (a) - censored slope vs OLS, order-of-magnitude check ──")
    print(f"  Slope summary:")
    for label, sl in slopes.items():
        print(f"    {label:18s} = {sl:.4f}")
    band_lo, band_hi = min(slopes.values()), max(slopes.values())
    print(f"  Range: [{band_lo:.4f}, {band_hi:.4f}]   (score-oracle worst case = 1.0)")
    if band_hi < 0.5:
        print(f"  → All censored estimators land ≪ 0.5 bits per bit, comfortably below the 1.0 score-oracle slope.")
        print(f"  → Order-of-magnitude claim (engine slope ≈ 0.05–0.07, vs worst-case 1.0) SURVIVES censoring correction.")
    elif band_hi < 0.99:
        print(f"  → All estimators give slope < 1; order-of-magnitude claim SURVIVES.")
    else:
        print(f"  → Censored slope crosses 0.99 - order-of-magnitude claim does NOT survive cleanly.")

    # ───────────────────────────────────────────────────────────────────
    # (2) Model comparison on the deadline-free subset (n=1093)
    # ───────────────────────────────────────────────────────────────────
    print()
    print("=" * 72)
    print(" (2) Model comparison on the deadline-free subset (no-CTX_X, n=1093)")
    print(" Observed bit range: ~10 – 120 bits")
    print("=" * 72)

    bits = df_no_ctx["bits"].values
    print(f"  bits   min = {bits.min():.1f}, median = {np.median(bits):.1f}, max = {bits.max():.1f}, n = {len(bits)}")

    # Build design matrices
    # (i) Exponential: log2(time) = a + b·bits
    X_exp = sm.add_constant(bits)
    # (ii) Power-law: log2(time) = c + d·log2(bits)
    X_pow = sm.add_constant(np.log2(bits))
    # (iii) Polynomial deg 2, 3 in bits
    X_poly2 = sm.add_constant(np.column_stack([bits, bits**2]))
    X_poly3 = sm.add_constant(np.column_stack([bits, bits**2, bits**3]))

    results = [
        fit_and_score(X_exp,   y_nc, 2, "Exponential   log2(t) = a + b·|p|"),
        fit_and_score(X_pow,   y_nc, 2, "Power-law     log2(t) = c + d·log2|p|"),
        fit_and_score(X_poly2, y_nc, 3, "Polynomial(2) log2(t) = a + b·|p| + c·|p|²"),
        fit_and_score(X_poly3, y_nc, 4, "Polynomial(3) log2(t) = a + b·|p| + … + d·|p|³"),
    ]

    print(f"\n  {'model':46s}  {'k':>2s} {'AIC':>8s} {'BIC':>8s} {'R²':>6s} {'RMSE_cv':>8s}")
    print("  " + "-" * 86)
    for r in results:
        print(f"  {r['label']:46s}  {r['k']:>2d} {r['AIC']:>8.1f} {r['BIC']:>8.1f} "
              f"{r['R2']:>6.3f} {r['rmse_cv']:>8.4f}")

    # Ranks by AIC and CV
    by_aic = sorted(results, key=lambda r: r["AIC"])
    by_cv  = sorted(results, key=lambda r: r["rmse_cv"])
    print(f"\n  Ranked by AIC:    {' > '.join(r['label'].split()[0] for r in by_aic)}")
    print(f"  Ranked by CV-RMSE:{' > '.join(r['label'].split()[0] for r in by_cv)}")

    # Likelihood ratio between exponential and power-law (same k, can't LRT; use AIC/BIC diffs)
    exp_aic = results[0]["AIC"]
    pow_aic = results[1]["AIC"]
    delta_aic_exp_vs_pow = exp_aic - pow_aic
    print(f"\n  ΔAIC (exponential − power-law) = {delta_aic_exp_vs_pow:.2f}")
    print(f"     positive → power-law preferred; negative → exponential preferred")

    # Verdict on (b)
    print()
    print("  ── Verdict (b) - exponential vs polynomial discrimination ──")
    best = by_aic[0]
    second_aic = by_aic[1]["AIC"]
    delta = second_aic - best["AIC"]
    discriminates = delta >= 10
    print(f"  Best model: '{best['label']}'  (AIC = {best['AIC']:.2f})")
    print(f"  ΔAIC to next model: {delta:.2f}  ({'> 10, strong preference' if discriminates else '≤ 10, weak discrimination'}).")
    if discriminates:
        print(f"  → The data DISCRIMINATES one model from the others over the bit range observed.")
    else:
        print(f"  → The data does NOT cleanly discriminate exponential from polynomial in this range.")
        print(f"     Both models fit comparably well; the observed bit range (10–120) is too narrow to separate them.")

    # ───────────────────────────────────────────────────────────────────
    # Save side-car
    # ───────────────────────────────────────────────────────────────────
    os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)
    with open(OUT_CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["section","model","n","n_censored","slope_or_metric","ci_or_se","extra"])
        # Section 1: censored fit results
        w.writerow(["1_censored", "OLS pooled", n_total, 0,
                    f"{ols_slope:.4f}", f"[{ols_ci[0]:.4f}, {ols_ci[1]:.4f}]",
                    f"R²={ols_pooled.rsquared:.4f}"])
        w.writerow(["1_censored", "OLS no-CTX_X (deadline-free)", n_nc, 0,
                    f"{ols_nc_slope:.4f}", f"[{ols_nc_ci[0]:.4f}, {ols_nc_ci[1]:.4f}]",
                    f"R²={ols_nc.rsquared:.4f}"])
        if aft_a is not None:
            w.writerow(["1_censored", f"AFT log-normal scheme A (≥{GLOBAL_CENSOR_S:.0f}s)",
                        aft_a["n"], aft_a["n_cens"],
                        f"{aft_a['beta_log2']:.4f}", f"[{aft_a['ci_log2_lo']:.4f}, {aft_a['ci_log2_hi']:.4f}]",
                        ""])
        if aft_b is not None:
            w.writerow(["1_censored", f"AFT log-normal scheme B (per-family p{PER_FAMILY_PERCENTILE:.0f})",
                        aft_b["n"], aft_b["n_cens"],
                        f"{aft_b['beta_log2']:.4f}", f"[{aft_b['ci_log2_lo']:.4f}, {aft_b['ci_log2_hi']:.4f}]",
                        ""])
        w.writerow(["1_censored", f"Tobit scheme B (per-family p{PER_FAMILY_PERCENTILE:.0f})",
                    tobit_b["n"], tobit_b["n_cens"],
                    f"{tobit_b['beta'][1]:.4f}", f"[{tobit_b['ci_lo'][1]:.4f}, {tobit_b['ci_hi'][1]:.4f}]",
                    f"sigma={tobit_b['sigma']:.4f}"])
        # Section 2: model comparison
        for r in results:
            w.writerow(["2_model_compare", r["label"], r["n"], 0,
                        f"AIC={r['AIC']:.2f}", f"BIC={r['BIC']:.2f}",
                        f"R²={r['R2']:.3f} CV-RMSE={r['rmse_cv']:.4f}"])
    print(f"\nwrote {OUT_CSV}")


if __name__ == "__main__":
    main()
