#!/usr/bin/env python3
"""
run_null_sweep.py  --  drive omnis_validate over the shuffled-null workload.

Reads null_shuffled.workload (one sequence per line), passes each through the
unchanged omnis_validate binary at the given --budget seconds, captures the
single CSV row each call produces, and accumulates them into --output.

Resumable: if --output already exists, sequences whose shuffled_id is already
present are skipped. The CSV schema matches the engine's --csv-header.

Usage:
    python3 run_null_sweep.py [--budget 600] [--output null_raw.csv] \
                              [--limit N] [--workload PATH]

For the smoke run we use --budget 60 and --limit 5. For the real sweep,
--budget 600 and no --limit (defaults to the full 300+ trials).
"""

import argparse, csv, os, subprocess, sys, time

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN          = os.path.join(REPO_ROOT, "build-cmake", "omnis_validate")
WORKLOAD     = os.path.join(ANALYSIS_DIR, "null_sweep", "null_shuffled.workload")
DEFAULT_OUT  = os.path.join(ANALYSIS_DIR, "null_sweep", "null_raw.csv")

# Columns omnis_validate emits — same as the baseline CSV schema.
SCHEMA = ["id","category","oeis_xref","A","total_n","train_n","k","sc","pred_sc",
          "solomonoff_class","mdl","raw_bits","ratio","time_s","solver_desc"]


def load_workload(path):
    seqs = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            seqs.append(line.rstrip("\n"))
    return seqs


def run_one(seq_line, budget):
    """Invoke omnis_validate on a single sequence line, return parsed CSV row."""
    # Pass the sequence on stdin (the engine reads from '-' or stdin).
    proc = subprocess.run(
        [BIN, "--budget", str(budget), "--freeze-db", "-"],
        input=seq_line + "\n",
        capture_output=True,
        text=True,
        timeout=budget + 60,  # safety margin above the budget
    )
    out = proc.stdout.strip().splitlines()
    # The engine prints a single CSV data row (no header) to stdout.
    csv_line = [ln for ln in out if "," in ln and not ln.startswith("#")]
    if not csv_line:
        raise RuntimeError(f"no CSV row in output:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    parts = next(csv.reader([csv_line[-1]]))  # last CSV-looking line
    if len(parts) != len(SCHEMA):
        raise RuntimeError(f"schema mismatch: got {len(parts)} cols, expected {len(SCHEMA)}\nline: {csv_line[-1]}")
    return dict(zip(SCHEMA, parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget",   type=int, default=600)
    ap.add_argument("--output",   default=DEFAULT_OUT)
    ap.add_argument("--workload", default=WORKLOAD)
    ap.add_argument("--limit",    type=int, default=None,
                    help="cap number of trials (for smoke runs).")
    args = ap.parse_args()

    if not os.path.isfile(BIN):
        sys.exit(f"missing engine binary {BIN} — build with cmake first")
    if not os.path.isfile(args.workload):
        sys.exit(f"missing workload {args.workload} — run null_make_workload.py")

    seqs = load_workload(args.workload)
    if args.limit is not None:
        seqs = seqs[:args.limit]
    ids = [s.split(" ", 1)[0] for s in seqs]
    print(f"will run {len(seqs)} trials, budget={args.budget}s each, "
          f"upper-bound wall time ~{len(seqs)*args.budget/3600:.1f}h")

    # Resumable: skip ids already present in the output.
    done = set()
    file_exists = os.path.isfile(args.output)
    if file_exists:
        for r in csv.DictReader(open(args.output)):
            done.add(r["id"])
        print(f"  resuming: {len(done)} trials already in {args.output}")

    mode = "a" if file_exists else "w"
    with open(args.output, mode, newline="") as out_f:
        w = csv.DictWriter(out_f, fieldnames=SCHEMA)
        if mode == "w":
            w.writeheader()
            out_f.flush()
        t_start = time.time()
        n_done = 0
        for i, seq in enumerate(seqs):
            sid = ids[i]
            if sid in done:
                continue
            t0 = time.time()
            try:
                row = run_one(seq, args.budget)
            except Exception as e:
                print(f"  [{i+1}/{len(seqs)}] {sid} ERROR: {e}")
                continue
            w.writerow(row)
            out_f.flush()
            n_done += 1
            wall = time.time() - t0
            sc, train_n = row["sc"], row["train_n"]
            pred_sc, k  = row["pred_sc"], row["k"]
            compressed  = int(sc) == int(train_n)
            predicted   = int(pred_sc) == int(k)
            verdict     = "DISCOVERY" if (compressed and predicted) else "no"
            print(f"  [{i+1}/{len(seqs)}] {sid:32s} t={wall:6.1f}s  "
                  f"sc/train={sc}/{train_n} pred/k={pred_sc}/{k}  -> {verdict}")
        elapsed = time.time() - t_start
        print(f"\ndone: {n_done} new trials in {elapsed:.0f}s; CSV at {args.output}")


if __name__ == "__main__":
    main()
