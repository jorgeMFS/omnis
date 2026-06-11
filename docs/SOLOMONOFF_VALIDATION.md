# Solomonoff classification, formally

This document is the formal specification of OMNIS's classification: how
each candidate sequence is placed into one of four cells based on whether
the engine's discovered program *compresses* its training prefix and
*predicts* its held-out continuation.

## 1. Setup

Let `S = (s_0, s_1, ..., s_{N-1})` be a finite alphabet sequence over
`{0, 1, ..., A-1}`. Split `S` into a training prefix and a held-out test
suffix:

- `K = max(K_min, ⌊N/4⌋)` with `K_min = 20`, fixed in advance and never
  tuned to outcomes.
- `train = (s_0, ..., s_{N-K-1})`, `train_N = |train|`
- `test  = (s_{N-K}, ..., s_{N-1})`, `|test| = K`

OMNIS runs `solve(train, A, deadline)` and obtains a `Res r` with score
`r.sc ∈ [0, train_N]` and description length `r.mdl ∈ ℝ_{≥0}`. From `r`
the engine derives a prediction sequence `pred[0..K)` either via
`runProgram(r, train_N + K, A)` (open-loop) or via the autoregressive
context path for `MODE_CTX` programs. The prediction match is
`pred_sc = #{ k : pred[k] == test[k] }`.

## 2. The two predicates

**Compresses** (the training prefix is faithfully encoded under MDL):

```
compresses(r, train, A) :=
    (r.sc == train_N) ∧ (r.mdl < train_N · log_2 A)
```

The first conjunct is exactness: the program reproduces the training
prefix without error. The second is *strict* compression: the program
must beat the raw bit-cost (`train_N · log_2 A`) of dumping the prefix
verbatim under uniform prior.

**Predicts** (the program's own continuation matches the held-out test):

```
predicts(r, test) := (pred_sc == K)
```

The criterion is strict equality (`pred_sc == K`), fixed in advance.
A program that compresses must extend
correctly *for every held-out symbol*, not just on average.

## 3. The Solomonoff contingency cell

Combining the two predicates gives a 2×2 contingency table:

|                    | predicts                       | does not predict          |
|--------------------|--------------------------------|---------------------------|
| **compresses**     | `discovered`                   | `compressed_only`         |
| **does not compress** | `not_compressed_predicted`  | `neither`                 |

The four cells are mutually exclusive and exhaustive. Every row in
`data/results/baseline_<date>.csv` carries one of these labels in column
`solomonoff_class`.

## 4. Why "discovered" is the gate

Solomonoff induction's prior over hypotheses is `P(h) ∝ 2^{-|h|}`, where
`|h|` is the description length of `h` under a chosen reference machine.
A program that compresses the training prefix is one such hypothesis;
its prior weight is `2^{-mdl(r)}`. If that hypothesis additionally
predicts every held-out symbol, the data is consistent with the
hypothesis being the *generator*, not merely a descriptor.

The combination — compression AND prediction — is the operational
analogue of "this hypothesis explains the past *and* extrapolates to
the future". Either alone is insufficient:

- `compressed_only` (compresses ∧ ¬predicts): the program memorised
  the training prefix but does not generalise. Common for highly
  specific programs (e.g. table-lookup-shaped).
- `not_compressed_predicted` (¬compresses ∧ predicts): the engine
  failed to find a short program but the candidate it returned happens
  to extend correctly. Bounded budget; no claim of generation.
- `neither`: the engine produced no exact match within budget;
  the prediction is a sentinel.

## 5. Concrete examples (from `benchmark14`)

| Sequence                   | sc / train_N | pred_sc / K | mdl  | raw_bits | class |
|----------------------------|-------------:|------------:|-----:|---------:|---|
| `bench_counting`           | 200/200       | 66/66       | 16.7 | 532.0    | discovered                |
| `bench_thuemorse`          | 256/256       | 85/85       | 16.7 | 341.0    | discovered                |
| `bench_pib4`               |   9/200       |  0/66       | 57.4 | 532.0    | neither                   |
| `bench_sigma`              |  50/50        | 20/20       | 102.3| 140.0    | not_compressed_predicted  |

`bench_sigma` is informative: the engine finds a 6-deep nested LOOP
that matches every held-out term but its 102.3-bit description exceeds
the raw 100-bit baseline (training prefix of 50 symbols at A=4). The
engine is reporting "I cannot describe this short" — appropriately.

## 6. Determinism contract

Across two runs with the same git SHA, OS, CPU, compiler, and per-
sequence budget:

- `(sc, pred_sc)` must be **identical** for every row.
- `solomonoff_class` must be **identical** for every row.
- `mdl` may vary by **up to 0.5 bits** (rounding + worker-pool order
  effects).
- `time_s` and `solver_desc` are advisory.

The second is the operational consequence of the first plus the strict
inequality in `compresses`: if `mdl` drift across reruns straddles the
strict-compression threshold (`mdl < train_N · log_2 A`), the cell can
flip even though `(sc, pred_sc)` is stable. We have not observed this
in practice on `benchmark14`; the default budgets are
conservative enough to keep `mdl` ≪ `raw_bits` on every `discovered`
sequence.

## 7. References

- `tools/omnis_validate.cpp` — the binary that computes the cell per row.
- Internal validation report — empirical validation on the 14-benchmark suite
  used here as `benchmark14`.
