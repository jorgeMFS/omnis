#!/usr/bin/env python3
"""
null_make_workload.py  --  build the shuffled-null workload.

For each row of null_sample.csv:
  1. Locate the source sequence (id + category) in data/categories/<cat>.txt.
  2. Permute its terms with a deterministic per-trial seed derived from the id.
     The multiset of symbols is preserved exactly; only the order is destroyed.
  3. Write the shuffled sequence into null_shuffled.workload, one line per trial
     in the same   <id> <A> <N> <t0> <t1> ... <t(N-1)>   format the engine
     consumes (see omnis_validate --help).

Also writes an audit manifest null_workload_audit.csv recording, per trial:
  sample_idx, original_id, shuffled_id, population, A, N, seed,
  original_multiset_sha, shuffled_multiset_sha, first5_orig, first5_shuf
The two multiset-shas must be equal (sanity check on the shuffle).
"""

import csv, os, sys, hashlib, random
from collections import Counter

REPO_ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SWEEP_DIR    = os.path.join(ANALYSIS_DIR, "null_sweep")
CAT_DIR      = os.path.join(REPO_ROOT, "data", "categories")
SAMPLE_CSV   = os.path.join(SWEEP_DIR, "null_sample.csv")
WORKLOAD     = os.path.join(SWEEP_DIR, "null_shuffled.workload")
AUDIT        = os.path.join(SWEEP_DIR, "null_workload_audit.csv")


def seed_for(original_id, salt=b"omnis-null-shuffled"):
    h = hashlib.sha256(salt + original_id.encode()).digest()
    return int.from_bytes(h[:8], "big")


def multiset_sha(terms):
    # Hash of the sorted symbol counts - invariant under permutation.
    cnt = sorted(Counter(terms).items())
    s = ",".join(f"{k}:{v}" for k, v in cnt)
    return hashlib.sha256(s.encode()).hexdigest()[:16]


def index_category(cat):
    """Build {id -> (A, N, terms[])} for one category file."""
    path = os.path.join(CAT_DIR, f"{cat}.txt")
    if not os.path.exists(path):
        sys.exit(f"missing workload file: {path}")
    idx = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            seq_id = parts[0]
            A      = int(parts[1])
            N      = int(parts[2])
            terms  = list(map(int, parts[3:3+N]))
            if len(terms) != N:
                sys.exit(f"corrupt line in {path}: {seq_id} expected {N} terms, got {len(terms)}")
            idx[seq_id] = (A, N, terms)
    return idx


def main():
    if not os.path.exists(SAMPLE_CSV):
        sys.exit("null_sample.csv missing - run null_sample.py first")

    sample = list(csv.DictReader(open(SAMPLE_CSV)))
    print(f"loaded {len(sample)} sampled sequences")

    cat_cache = {}
    audit_rows = []
    out_lines  = []
    for r in sample:
        cat = r["category"]
        if cat not in cat_cache:
            cat_cache[cat] = index_category(cat)
            print(f"  indexed {cat}: {len(cat_cache[cat])} sequences in source workload")
        idx = cat_cache[cat]
        orig_id = r["original_id"]
        if orig_id not in idx:
            sys.exit(f"id {orig_id} (population {r['population']}) not in {cat}.txt")
        A_src, N_src, terms = idx[orig_id]
        if int(r["alphabet_size"]) != A_src or int(r["total_n"]) != N_src:
            sys.exit(f"id {orig_id}: A/N from CSV differ from workload")

        # Shuffle deterministically.
        seed = seed_for(orig_id)
        rng  = random.Random(seed)
        shuf = terms[:]
        rng.shuffle(shuf)

        shuf_id = f"{orig_id}_shuf"
        out_lines.append(f"{shuf_id} {A_src} {N_src} " + " ".join(map(str, shuf)) + "\n")
        audit_rows.append({
            "sample_idx":            r["sample_idx"],
            "original_id":           orig_id,
            "shuffled_id":           shuf_id,
            "population":            r["population"],
            "category":              cat,
            "A":                     A_src,
            "N":                     N_src,
            "seed":                  seed,
            "original_multiset_sha": multiset_sha(terms),
            "shuffled_multiset_sha": multiset_sha(shuf),
            "first5_orig":           " ".join(map(str, terms[:5])),
            "first5_shuf":           " ".join(map(str, shuf[:5])),
        })

    # Verify multisets are invariant.
    bad = [a for a in audit_rows if a["original_multiset_sha"] != a["shuffled_multiset_sha"]]
    if bad:
        sys.exit(f"FATAL: {len(bad)} shuffles changed the multiset (bug)")

    with open(WORKLOAD, "w") as f:
        f.write(f"# omnis null-sweep workload (shuffled)\n")
        f.write(f"# generator: analysis/null_make_workload.py\n")
        f.write(f"# count: {len(out_lines)}\n")
        for line in out_lines:
            f.write(line)

    with open(AUDIT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(audit_rows[0].keys()))
        w.writeheader()
        w.writerows(audit_rows)

    print(f"\nwrote {WORKLOAD}  ({len(out_lines)} shuffled sequences)")
    print(f"wrote {AUDIT}")
    print("multiset invariance: PASS (no shuffle changed the symbol counts)")


if __name__ == "__main__":
    main()
