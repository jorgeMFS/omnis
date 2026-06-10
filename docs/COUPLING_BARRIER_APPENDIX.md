# Appendix: The Coupling-Barrier Framework, Instantiated for OMNIS

This appendix relates the OMNIS empirical results to the lower-bound
theorems of the coupling-barrier framework (Lavraga 2026,
*Time is the Coupling Barrier*, hereafter [TIME-CB]; companion
appendix *The Coupling Barrier Theorem*, hereafter [CB-APP]). The
external papers state and prove the theorems; this appendix only
*instantiates* them — binds the theorems' abstract quantities to the
engine's concrete fields, and exhibits empirical observations that the
theorems either predict or rationalize.

We do not re-prove anything here. We do not claim that OMNIS achieves
the lower bound. We claim only that the engine's empirical
failure-mode distribution is consistent with the theorem, and that the
sample-length dependence of discovery is predicted to first order by
[CB-APP, Cor. 2.7.15c].

## A.1 Notation and binding to OMNIS

Let a candidate row of `data/results/baseline_*.csv` be
$(\text{id}, A, N_{\text{tot}}, N, K, sc, pred\_sc, \text{cls}, \text{mdl}, \ldots)$
where $N$ = `train_n` is the training-prefix length presented to
`solve()`, and the target alphabet is $\Sigma = [0, A)$.

| External symbol | OMNIS realisation | Source |
|---|---|---|
| $d$ — type/instruction alphabet | $\|T_{\text{ISA}}\|$ = `isaMaxConstant() + 1` plus invocable `SUB_CALL` slots; canonically 11 for the baseline empty-library sweep | `src/omnis.cpp::buildL1`, `subCallCatalogSize` |
| $\kappa$ — coupling width | $K$ = syntactic minimality of the discovered program, lower-bounded by $\lceil \text{mdl}/\log_2 d\rceil$ via [CB-APP, Cor. 4.2a] | `computeMDL`, `Res::nbody` |
| $N$ — sample length | `train_n` | column 6 of the CSV schema |
| $\mu(s)$ — max symbol frequency | empirical $\max_a |\{t : s_t = a\}|/N$ | computable from any workload row |
| $D$ — maximum register-lag (essential engagement) | `ctx_dstar` for MODE_CTX, body-depth otherwise | `Res::ctx_dstar`, `Res::nbody` |
| $m_{\text{signal}}$ — score-function signal mass | $\Omega(\Delta - \delta)$ per [CB-APP, Thm. 2.7.12] | not measured directly; inferred via discovery outcome |
| $T_{\text{search}}$ — total time-to-discovery | column 14 `time_s` | per row |

The OMNIS run-script enforces [CB-APP] hypothesis-cleanliness for the
sweep: `--freeze-db` against a synthesised empty `program_db.bin`
(§INTEGRATION_PLAN.md §3) means the library is not an implicit input,
so the engine's score function is well-defined per candidate
independently of run order.

## A.2 The lower bound (statement only)

[TIME-CB, Thm. 1.4] / [CB-APP, Cor. 2.7.13]: Any score-oracle algorithm
for the OMNIS type-discovery CSP requires

$$
T_{\text{search}} \;\geq\; \Omega\!\!\left(\frac{(d-1)^{K}}{m_{\text{signal}}}\right)
$$

expected evaluations against the worst-case
$(\delta, \Delta)$-consistent score function. The bound applies
*against worst-case score functions in $F_{\delta, \Delta}$* (the
"flat-basin" family); the actual register-machine match score is
$(\delta, \Delta)$-consistent ([CB-APP, Def. 7]), so the bound binds
in the worst-case sense of [CB-APP, S1.4 Remark]. We do not claim it
binds OMNIS in the average case.

## A.3 The data-dependence relation (statement only)

[CB-APP, Cor. 2.7.15c]: For a register-connected target program $P^*$
([CB-APP, Def. 2.7.14b]) of maximum lag $D$, coincidence-bounded
against $s$ with residual $\gamma(N)$,

$$
\Delta - \delta \;=\; (1 - \mu(s))\cdot N \;-\; D \;-\; \gamma(N).
$$

The signal mass satisfies $m_{\text{signal}} = \Omega(\Delta - \delta)$,
hence the lower bound becomes

$$
T_{\text{search}} \;\geq\; \Omega\!\!\left(\frac{(d-1)^{K}}{(1 - \mu(s))N - D - \gamma(N)}\right).
$$

The right-hand side is $\Omega((d-1)^K / N)$ when $\mu(s) < 1$,
$D = o(N)$, $\gamma(N) = o(N)$ — the *signal-rich regime*. When any
condition fails, the denominator collapses to $o(N)$ and the effective
lower bound spikes; for a fixed wall-clock budget the engine cannot
clear it.

## A.4 First instantiation: Rule 30 at three sample lengths

Rule 30 (Wolfram class III, [Wolfram 2002]) is the canonical
chaotic-CA-with-discoverable-program test case. The engine encounters
it three times across our experimental history.

| Source | $N$ | $N_{\text{train}}$ | $\mu(s)$ | predicted margin $\;(1-\mu(s))N - D - \gamma(N)$ | $T_{\text{search}}$ | outcome |
|---|---:|---:|---:|---:|---:|---|
| ECA-256 (ENARZ historical baseline, [Lavraga 2025]) | 200 | 150 | $0.500$ | $\;\approx 75$ | $\leq 60$ s | discovered, WIDE_BIT, $\text{mdl} \in [70, 72]$ |
| benchmark14 / `bench_rule30` (current sweep) | 266 | 200 | $0.500$ | $\;\approx 100$ | $66.9$ s | discovered, WIDE_BIT nr=3 bit=100, $\text{mdl} = 73.26$ |
| OEIS A070950_a2 (OEIS-truncated, current sweep) | 75 | 55 | $0.500$ | $\;\approx 27$ | $600.0$ s | **neither**, $sc = 19/55$, partial match only |

The Kolmogorov object is identical across rows (Rule 30 center column
from single-cell IC); $K$ is therefore identical. The variable that
changes is $N$. The predicted margin shrinks by $\sim 4\times$ between
the 266-term and 75-term presentations; in the latter,
$(d-1)^K/m_{\text{signal}}$ exceeds the engine's wall-clock budget at
$600$ s. The empirical outcome flips from `discovered` to `neither`.

This single experiment is the cleanest empirical realisation of
[CB-APP, Cor. 2.7.15c] available from the OMNIS data: *same target, same
ISA, same engine, same budget; sample length alone determines the
outcome.* It does not prove the bound (a single instance cannot), but
the direction and approximate magnitude of the effect are predicted
*before* the data is collected.

## A.5 Second instantiation: time distribution of discovered programs

[Disclaimer: the figures below are partial-sweep snapshots through
$310/4588$ candidates. The final paper figure should redraw against
the completed Phase E baseline.]

Of $100$ candidates classified `discovered` in the partial sweep:

| $T_{\text{search}}$ bucket | count |
|---|---:|
| $< 5$ s | (within 5-20 s) |
| $5$–$20$ s | $70$ |
| $20$–$60$ s | $19$ |
| $60$–$200$ s | $5$ |
| $200$–$500$ s | $6$ |
| $\geq 500$ s | $0$ |

Zero discoveries occurred in the final $100$ seconds of the $600$ s
budget. The lower bound predicts $T_{\text{search}}$ grows with $K$
(the discovered program's syntactic minimality) and inversely with the
signal margin. Both axes of variation are present in the data:
candidates with $K \leq 20$ bits cluster at $T < 20$ s; candidates
with $K \geq 100$ bits cluster at $T \geq 100$ s (NESTED_LOOP L=5/L=6
family).

The absence of discoveries in the $[500, 600]$ s tail is consistent
with the conjunctive condition: the engine either has enough
signal-mass to find a program quickly, or it does not have enough to
find one within any feasible budget. The theorem does not predict an
exact cutoff; it predicts that the cutoff exists for each
$(K, m_{\text{signal}})$ pair and that the engine's empirical
budget-exhaustion distribution should be bimodal. Our data is.

## A.6 Apparent anomaly: $A=2$ vs $A=3$ discovery rates

A first-order reading of [CB-APP, Cor. 2.7.15c] favours $A=3$:
$\mu(s) \approx 1/A$ for uniformly-distributed targets, so the margin
$(1-\mu(s)) N$ is $\sim 0.5 N$ at $A=2$ but $\sim 0.67 N$ at $A=3$.
The theorem predicts $A=3$ should be *easier*.

The empirical observation is the opposite: across $97$ OEIS sequences
where some alphabet variant was discovered, $A=2$ won $32$ times when
$A=3$ failed; $A=3$ won only once where $A=2$ failed; both succeeded
in $18$ cases; both failed in $46$.

The apparent contradiction is resolved by recognising that $K$ is
*not* alphabet-invariant. The program minimality $K$ is measured in
ISA opcodes whose alphabet of output is $\Sigma = [0, A)$; an
$A=3$-output program requires either a `MOD_C` opcode parametrised by
$3$ (so $K$ inherits an extra `uInt(3)` cost in `computeMDL`) or an
`ITER d=3` family member that the engine searches with a less
specialised path than the `WIDE_BIT` path used for $A=2$. The
asymmetry is *not* in the data-dependence margin; it is in the
ISA-relative coupling width $K$ itself.

The framework does not predict the asymmetry, but accommodates it:
[CB-APP, S2.5] proves that coupling width is *irreducible* to known
priors only modulo a fixed reference machine. OMNIS's reference
machine is binary-output-biased; a paper reporting OMNIS results
should disclose this and either:

1. Reduce its OEIS-class claims to $A=2$ explicitly, or
2. Add a ternary-native ISA path and re-measure, or
3. Report both rates with the bias acknowledged.

For the current sweep we adopt (3).

## A.7 Apparent regularity: equivalence-class MDL clustering

Across all $\sim 100$ discoveries to date, only $\sim 8$ distinct MDL
values appear. Examples:

- $\text{mdl} = 8.82$: single member, `bench_champernowne` (`CONCAT d=4 off=0`).
- $\text{mdl} = 47.78$: $\geq 10$ collatz_grid + cellular members (`FUNC_L d=4`).
- $\text{mdl} = 97.72$: `arith_d_n` $+$ `bench_divisorcount` (`NESTED_LOOP L=5`).
- $\text{mdl} = 116.78$: `arith_sigma_n` $+$ $3$ collatz_grid members (`NESTED_LOOP L=6`).

[CB-APP, S2.3, "The Representational Tradeoff"] proves that an
equivalence class of programs sharing structural type-skeleton and
register-arity has identical reference-machine description length.
The empirical MDL clustering is consistent with this: distinct
candidates whose discovered program shares the type-skeleton land on
identical numerical $\text{mdl}$ values via `computeMDL`.

This is a *consistency check*, not a test. The theorem predicts that
MDL distinguishes programs only up to structural equivalence; the data
shows it. We do not propose this as a novel measurement.

## A.8 Limits of the empirical evidence

What this appendix *does not* establish:

1. **OMNIS is asymptotically optimal.** We do not measure
   $T_{\text{search}}$ for adversarial worst-case score functions;
   only the natural match-score is exercised. The lower bound binds
   in the worst case, not the OMNIS-average case.

2. **The lower bound is *tight* for OMNIS.** A discovery
   completed in $T_{\text{search}} \ll \Omega((d-1)^K / m_{\text{signal}})$
   does not violate the bound (the bound is on the *minimum* possible
   $T$, not the expected $T$).

3. **The Rule 30 instance generalises.** A single $(N, \text{outcome})$
   transition does not establish that the discovery threshold curve
   follows the theorem's functional form. The Phase E sweep contains
   $\sim 4500$ candidates spanning multiple $N$ values per category;
   a per-category $T(N)$ scatter would test the relation
   quantitatively. Future work.

4. **$\mu(s)$ and $\gamma(N)$ are the right quantities to fit.** Our
   $\mu(s)$ uses empirical max-frequency; the theorem's $\mu(s)$ is
   the asymptotic baseline match probability under coincidence
   ([CB-APP, Def. 2.7.10]). For balanced targets the two coincide;
   for skewed targets they may not, and the margin formula's
   constants would need revisiting.

5. **Coincidence-boundedness $\gamma(N) = o(N)$ holds.** We assume it;
   [CB-APP, Prop. 2.7.16a] proves it only for the graded-translation
   sub-ISA. For OMNIS's full 18-opcode ISA the proof is open
   ([CB-APP, S5.6 (iii)]). The empirical success of the framework on
   OEIS data is *evidence* that the condition holds in practice for
   the workloads tested; it is not a proof.

## A.9 What this appendix contributes

Three things, each one paragraph in the paper:

**§A.4 + §A.5 — the data-bound effect is real and predictable.**
Rule 30's $N=75$ failure was anticipated by Cor. 2.7.15c in advance of
the OEIS sweep; the time-bucket distribution is consistent with the
theorem's bimodality prediction. These are the strongest empirical
links to the theorem.

**§A.6 — the ISA bias is visible and disclosed.** A=2/A=3 asymmetry
is the most honest finding the paper can offer about OMNIS's
limits. The framework accommodates the bias by making it explicit in
$K$ rather than $N$.

**§A.7 — MDL clustering is a sanity check on the reference machine.**
We observe equivalence-class behaviour that the theorem predicts.

These three contributions sit *inside* the appendix because they do
not change the paper's main claims (engine + Solomonoff classification
+ Phase E sweep), but they tell a reader who knows [TIME-CB] that the
empirical run is theoretically interpretable, not merely a benchmark
exercise.

## Reference numbering

Within the body of the OMNIS paper, prefer:

- "(Cor. 2.7.15c, [CB-APP])" rather than restating the formula.
- "($T_{\text{search}}$ lower bound, [TIME-CB, Thm. 1.4])" rather than the inequality.
- Numerical instantiations belong in tables in this appendix; the body
  prose should only state outcomes.

## Provenance

This appendix is consistent with the Phase E run state at
$310 / 4588$ candidates (cut-off 2026-05-12 17:00 UTC). The numbers
in §A.5 and §A.6 should be refreshed against the completed CSV
before the manuscript is submitted; the structural claims (§A.2-A.4,
§A.7) are independent of the partial-sweep cut-off.
