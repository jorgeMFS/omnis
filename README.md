# OMNIS

A program-search engine for integer sequences. Given a sequence, OMNIS searches a canonical 18-opcode register-machine ISA for a short program that reproduces the sequence exactly, ranking candidates by Minimum Description Length (MDL).

OMNIS = Observational Minimal-length Nonparametric Inductive Synthesis.

## Architecture

- `src/omnis.cpp` — the engine, as a single translation unit. No `main()`. Anyone can include it from their own driver.
- `src/cli.cpp` — a minimal command-line wrapper around the engine. ~190 lines.

The engine and the CLI are separate. The engine is the contribution; the CLI is a thin demonstration tool.

## Build

CMake (recommended):

```bash
mkdir build-cmake && cd build-cmake
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

Or directly:

```bash
./build/build.sh        # plain build
./build/build_pgo.sh    # profile-guided optimisation (~10–15% speedup)
```

## Usage

`omnis` reads one sequence from a file or from stdin. Format:

```
<id> <A> <N> <t0> <t1> ... <t(N-1)>
```

`A` is the alphabet size; `N` is the number of terms. `'#'` at line start is a comment.

```bash
# From stdin
echo "demo 4 8 0 1 2 3 0 1 2 3" | ./omnis -

# From a file (one sequence per file)
./omnis path/to/sequence.txt
```

For multi-sequence workloads, compose with shell:

```bash
while read line; do echo "$line" | ./omnis -; done < workload.txt > results.tsv
```

A small wrapper `build/mine.sh` is included for the same purpose.

Options:

| Flag | Meaning |
|---|---|
| `--budget <s>` | Wall-clock budget in seconds (default 600) |
| `--freeze-db` | Read the program library but do not add new entries |
| `--db <path>` | Program database path (default `../data/program_db.bin`) |
| `--json` | Machine-readable output |
| `-h`, `--help` | Show usage |

Exit codes: `0` solved, `1` partial, `2` usage, `3` I/O.

## Tests

```bash
cd build-cmake && ctest --output-on-failure
```

Six test suites covering SUB_CALL semantics, Phase 2H pool building, Phase 2F budget enforcement, regression coverage for previously-found bugs, engine determinism (two runs must agree exactly on scores and classification), and the categorical workload pipeline (`gen_workload` + `omnis_validate` + frozen-checksum verification).

## Repository layout

```
src/        engine + CLI (omnis.cpp, cli.cpp)
tests/      regression tests + benchmark generators (used by tests only)
build/      build scripts (build.sh, build_pgo.sh, mine.sh)
data/       workload files, OEIS snapshot pins, program database,
            and the sweep results under data/results/: the four paper
            populations plus a smoke run (benchmark targets and the
            twenty negative controls, none of which pass the gate)
analysis/   everything behind the paper's figures and tables: the
            per-discovery and null-calibration datasets, the Python
            analysis scripts, the R figure scripts, and the final
            rendered outputs (see analysis/README.md)
docs/       architecture + integration documentation
tools/      auxiliary binaries and scripts:
              gen_workload    — deterministic categorical workload generator
              oeis_loader     — OEIS-snapshot streamer with named filters
              omnis_validate  — train/test wrapper with Solomonoff classification
              gen_all.sh      — regenerate every committed workload file
              oeis_fetch.sh   — fetch + SHA-pin OEIS snapshot
```

In addition to the main `omnis` CLI, the build produces `omnis_validate`,
`gen_workload`, and `oeis_loader` for reproducible categorical sweeps. See:

- `docs/REPRODUCING.md` — clone → build → fetch → run → CSV recipe
- `docs/CATEGORIES.md` — workload-category overview + pre-registered targets
- `docs/SOLOMONOFF_VALIDATION.md` — formal classification specification
- `docs/ARCHITECTURE.md` — engine internals (instruction set, modes, MDL)

## Citation

`CITATION.cff` provides citation metadata. After publication of the accompanying paper, this README will record the citation.

## License

MIT — see `LICENSE`.

## Status

Research code accompanying the paper. The engine, the workloads, the sweep
results and the analysis pipeline in this repository reproduce every number
and figure in the manuscript.
