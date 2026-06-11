# OMNIS Architecture

This document describes the implementation of the OMNIS engine. For the user-facing CLI, see `README.md`. For citation metadata, see `CITATION.cff`.

## 1. Source layout

```
src/omnis.cpp     ~4100 lines, single translation unit. Contains the engine.
src/cli.cpp       ~190 lines. Reads one sequence, calls solve(), prints result.
```

`omnis.cpp` is a single self-contained translation unit. The public-by-convention surface is:

| Function / type | Purpose |
|---|---|
| `Res solve(const std::vector<int>& tgt, int A, double dl)` | Search for a program reproducing `tgt` over alphabet `A` within deadline `dl`. |
| `std::vector<int> runProgram(const Res& r, int K, int A)` | Execute a discovered program and return its first `K` outputs. |
| `int predictNext(const Res& r, const std::vector<int>& tgt, int A)` | Predict the symbol at position `N = tgt.size()`. |
| `double computeMDL(const Res& r, int ncat)` | MDL bit-cost of a program under canonical encoding. |
| `ProgramDB g_progdb` | The persistent program library. |
| `Res`, `Ins`, `ProgramRecord` | Result, instruction, and library-record types. |

External callers can `#include "omnis.cpp"` directly and link the resulting object. There is no separate header: the file is its own library.

## 2. Instruction set

Eighteen primitive opcodes plus one hierarchical opcode:

| `ti` | Mnemonic | Arity | Semantics |
|---:|---|:---:|---|
| 0 | INC | 1 | `R[a]++` (saturating) |
| 1 | DEC | 1 | `R[a]--` (clamped at 0) |
| 2 | ADD | 3 | `R[c] = sat(R[a] + R[b])` |
| 3 | SUB | 3 | `R[c] = sat(R[a] - R[b])` (clamped) |
| 4 | MUL | 3 | `R[c] = smul(R[a], R[b])` |
| 5 | MUL_C | 2+c | `R[b] = smul(R[a], c)` |
| 6 | MOD_C | 2+c | `R[b] = R[a] mod_E c` |
| 7 | MOD_R | 3 | `R[c] = R[a] mod_E R[b]` |
| 8 | DIVC | 3+c | `R[b] = R[a]/c; R[c'] = R[a] mod_E c` |
| 9 | DIVR | 4 | Register-divisor div+mod |
| 10 | LOAD | 1+c | `R[a] = c` |
| 11 | COPY | 2 | `R[b] = R[a]` |
| 12 | OUT | 2 | Emit `R[a] mod g_emit_A` (EMIT mode only) |
| 13 | AND | 3 | bitwise (non-negative operands) |
| 14 | OR | 3 | bitwise |
| 15 | XOR | 3 | bitwise |
| 16 | ISZERO | 2 | `R[b] = (R[a] == 0) ? 1 : 0` |
| 17 | LOOP | 1+c | Repeat preceding `c` instructions while `R[a] != 0`, max 200 iterations |
| 32 | SUB_CALL | 0 | Inline-expand library entry `c` |

`mod_E` denotes Euclidean modulus (always non-negative). All arithmetic is saturating with bound `SAT = 1e15`. The thread-local flag `g_sat` is set on overflow; saturated programs are rejected.

There are 8 general-purpose registers `R[0..7]`. SUB_CALL invokes a previously-discovered library entry whose body is purely primitive (no nested SUB_CALL - enforced by a recursion guard at catalog-build time).

## 3. Execution modes

A discovered program (`Res`) carries an execution mode:

- **MODE_ITER** - `R = init`, persistent across iterations; output is `R[outr] mod A` per iteration.
- **MODE_FUNC** - `R = {n, 0, ..., 0}` per iteration `n`; output is `R[outr] mod A`.
- **MODE_EMIT** - body emits via OUT; collected in `g_emit_buf`.
- **MODE_CTX** - registers reload from target history each iteration (used by Phase 1B's deductive context search).

Plus a small set of "deductive" forms used by certain accelerator phases:

- **CONCAT** - base-`b` digit concatenation generator (parameters `concat_base`, `concat_off`, `concat_msb`).
- **DARY** - base-`b` per-digit recurrence (parameters `dary_base`, `dary_init_val`, `dary_op`).
- **STEP** - count-iterations-to-halt wrapper around a branched body.

## 4. MDL encoding

`computeMDL(r, ncat)` returns the program's description length in bits under the canonical encoding. The formula sums:

- A 2-bit mode tag.
- `bodyMDL`, computed per instruction as `log2(ncat) + ar * log2(nr)` plus `uInt(c)` for LOOP-length.
- For branched programs: 1 flag bit + `uInt(branch_m)` + two body-cost terms.
- For OUT_BIT output: 1 flag bit + `uInt(bit_pos)`.
- Init values for ITER/EMIT modes: `uInt(nr) + Σ uInt(init[i]) + log2(nr)`.
- For MODE_CTX: `uInt(ctx_dstar) + uInt(nr) + nr * log2(ctx_dstar) + log2(nr)`.
- For CONCAT/DARY/STEP: explicit encoding of mode-specific parameters.

`ncat` is the total catalog size (canonical opcode constant ranges plus invocable library entries). It is computed once per `solve()` call and is stable for the duration of that call.

`uInt(n)` is universal integer coding: `log2*(n) + log2(n+1)`.

## 5. Search phases

`solve()` runs seven phases sequentially. Each phase produces an updated `best` `Res` and may record additional candidates into `g_progs` (the per-call mixture pool).

### Phase 0 - Library extension (~ms)

If the persistent library is non-empty, `g_progdb.testExtensions()` tests extensions of stored programs against the target. Cheap deductive pre-filter.

### Phase 1 - T-table deduction (instant)

`isaMatchUnary`, `isaMatchBinary`, `isaMatchPair`, `isaMatchBranched`, and `verifyCTX` deduce programs whose structure is captured by simple algebraic relations on observed transitions.

### Phase 2A - Flat sieve (sub-second to seconds)

Enumerate primitive bodies of length L=1..3 and verify against the target. Pool construction via `buildDDB` and `composeDDB`.

### Phase 2B - Branched cascade (seconds)

`cascadeSearch` finds programs of the form `if R[0] mod m == 0 then body_then else body_else`. Searches over `m ∈ {2..min(A,10)}` and pre-body pools.

### Phase 2C - Wide-bit sieve (binary alphabet only)

For `A == 2`, programs that operate on 512-bit packed integers (`W` type) and extract bit `bit_pos`. Useful for cellular-automaton-class targets.

### Phase 2F - Unified WSBP, wire-space backward propagation (deep search)

Enumerates type-tuples for body lengths L=1..10, propagates demand wires backward, probes loop-counter registers, and tests every (pre-body, inner body, mode) combination. The phase has a per-level Levin budget allocation and a hard deadline derived from the per-target wall budget.

### Phase 2H - Hierarchical synthesis

Builds compose-pools `p2..p8` of bodies containing at least one SUB_CALL slot, then tests each composed body in MODE_FUNC, MODE_ITER, and MODE_EMIT. Uses a witness-trajectory pre-filter to skip MODE_FUNC enumerations that cannot match `tgt[0]`. Tests are gated by a deadline check between modes to bound per-candidate runtime.

## 6. Persistent program library (`ProgramDB`)

`g_progdb` holds discovered programs across runs. The on-disk format is a small versioned binary:

```
"ENAR" magic (4 bytes)
version (4 bytes, currently 2)
count (4 bytes)
N × ProgramRecord (sizeof(ProgramRecord) bytes each)
```

The library is loaded at `solve()` entry and (under default behaviour) saved when a new program is recorded. The `--freeze-db` CLI flag suppresses writes - useful for evaluation runs that need a stable catalog.

Dedup at insertion uses `programRecordHash`, which hashes all fields that distinguish programs structurally (body, mode, registers, init values, branch parameters, special-mode parameters).

## 7. Concurrency

Most search phases use a thread pool sized at `std::thread::hardware_concurrency()`. Critical shared state:

| State | Protection |
|---|---|
| `g_progs` | `g_progs_mutex` |
| `g_progs_fingerprints` | `g_progs_mutex` |
| `g_sat`, `g_emit_A`, `g_emit_buf`, `g_subcall_cache` | `thread_local` |
| `phase2_collecting`, `p2c_dl_get/set` | `std::atomic` |
| `best` (per-phase) | per-phase mutex |
| `g_progdb` | read-only during search; writes serialized at solve() boundary |

The SUB_CALL memoization cache is per-thread (`g_subcall_cache`), cleared at `solve()` entry, and disabled when `g_emit_A > 0` (otherwise OUT side-effects in cached invocations would be skipped).

## 8. Build

CMake (`CMakeLists.txt`) builds one binary, `omnis`, from `src/cli.cpp` (which `#include`s `src/omnis.cpp`). Tests live under `tests/` and are wired into CTest.

A small set of shell scripts under `build/`:
- `build.sh` - direct g++ build
- `build_pgo.sh` - profile-guided optimization
- `mine.sh` - shell loop over a workload file (composes `omnis -` per line)

## 9. Tests

Six test files under `tests/`:

| Suite | Coverage |
|---|---|
| `test_omega_sub_call.cpp` | SUB_CALL semantics: invocability filter, recursion guard, branched filter, MODE_CTX filter, catalog count |
| `test_phase2h_simple.cpp` | Phase 2H pool building (p2/p3/p4/p5) and SUB_CALL slot containment |
| `test_phase2f_budget.cpp` | Phase 2F deadline enforcement (with and without library; dormant path) |
| `test_correctness_fixes.cpp` | Regression coverage for fixes to bugs found during development (cross-mode cache, indirect-OUT detection, fingerprint completeness, MDL coverage) |
| `test_engine_determinism.cpp` | Determinism canary: two runs over the reference benchmarks must agree on `(sc, pred_sc, class)` with `mdl` within ±0.5 bits |
| `test_gen_workload.cpp` | Workload generator: format, determinism under fixed `SOURCE_DATE_EPOCH`, checksum stability, CSV schema of `omnis_validate` |

Tests `#include "omnis.cpp"` directly to exercise internal functions. Build via CTest:

```bash
cd build-cmake && ctest --output-on-failure
```

## 10. Notes on the CLI

`src/cli.cpp` reads from the file / stdin / `--terms` CSV inputs described in `README.md`, calls `solve()`, and prints the result. It depends only on the public-by-convention API listed in §1.

For multi-sequence workloads, `cli.cpp` does not contain a built-in loop; users compose with shell. The `build/mine.sh` wrapper is one such composition.
