#!/usr/bin/env python3
"""
generate_candidates.py - Generate input files for oeis_miner.

Usage:
    python3 generate_candidates.py D > candidates_D.txt   # p-adic valuations
    python3 generate_candidates.py C > candidates_C.txt   # mod-reductions
    python3 generate_candidates.py E > candidates_E.txt   # Stern-Brocot / digit-counting
    python3 generate_candidates.py B > candidates_B.txt   # automatic sequences (downloads)
    python3 generate_candidates.py F > candidates_F.txt   # broad mining (downloads)
"""

import sys
import math

def emit(oeis_id, A, terms):
    """Print one input line: OEIS_ID A N term0 term1 ..."""
    N = len(terms)
    print(f"{oeis_id} {A} {N} " + " ".join(str(t) for t in terms))


# =========================================================================
# Category D: p-adic valuations
# =========================================================================

def v_p(n, p):
    """p-adic valuation of n: largest k such that p^k divides n."""
    if n == 0:
        return 0
    k = 0
    while n % p == 0:
        k += 1
        n //= p
    return k

def v_p_factorial(n, p):
    """p-adic valuation of n! via Legendre's formula."""
    s = 0
    pk = p
    while pk <= n:
        s += n // pk
        pk *= p
    return s

def binomial(n, k):
    """Binomial coefficient C(n, k)."""
    if k < 0 or k > n:
        return 0
    if k == 0 or k == n:
        return 1
    k = min(k, n - k)
    result = 1
    for i in range(k):
        result = result * (n - i) // (i + 1)
    return result

def gen_category_D(N=500):
    """p-adic valuations (~50 lines)."""
    primes = [2, 3, 5, 7]
    oeis_ids = {"2": "A007814", "3": "A007949", "5": "A112765", "7": "A214411"}
    A_values = [2, 3, 4, 5, 8]

    # v_p(n) for n=1..N
    for p in primes:
        base_id = oeis_ids.get(str(p), f"SYNTH_v{p}")
        terms_raw = [v_p(n, p) for n in range(1, N + 1)]
        for A in A_values:
            terms = [t % A for t in terms_raw]
            oid = base_id if A > max(terms_raw[:N]) else base_id
            emit(f"{base_id}_A{A}", A, terms)

    # v_p(n!) for n=0..N
    fact_ids = {"2": "A011371", "3": "A054861", "5": "SYNTH_v5_fact", "7": "SYNTH_v7_fact"}
    for p in [2, 3, 5]:
        base_id = fact_ids[str(p)]
        terms_raw = [v_p_factorial(n, p) for n in range(N)]
        for A in A_values:
            terms = [t % A for t in terms_raw]
            emit(f"{base_id}_A{A}", A, terms)

    # v_2(C(2n,n)) and v_3(C(2n,n)) for n=0..200
    N_binom = 200
    for p in [2, 3]:
        terms_raw = [v_p(binomial(2 * n, n), p) for n in range(N_binom)]
        for A in [2, 3, 4]:
            emit(f"SYNTH_v{p}_cbc_A{A}", A, [t % A for t in terms_raw])


# =========================================================================
# Category C: Mod-reductions of classical sequences
# =========================================================================

def gen_partition_mod(N, m):
    """Partition numbers p(n) mod m via pentagonal number theorem."""
    p = [0] * (N + 1)
    p[0] = 1
    for n in range(1, N + 1):
        s = 0
        k = 1
        while True:
            # Generalized pentagonal numbers: k(3k-1)/2 and k(3k+1)/2
            g1 = k * (3 * k - 1) // 2
            g2 = k * (3 * k + 1) // 2
            if g1 > n:
                break
            sign = 1 if k % 2 == 1 else -1
            s += sign * p[n - g1]
            if g2 <= n:
                s += sign * p[n - g2]
            k += 1
        p[n] = s % m
    return p[:N]

def gen_catalan_mod(N, m):
    """Catalan numbers C(n) mod m."""
    c = [0] * N
    c[0] = 1
    for n in range(1, N):
        s = 0
        for k in range(n):
            s = (s + c[k] * c[n - 1 - k]) % m
        c[n] = s
    return c

def gen_bell_mod(N, m):
    """Bell numbers B(n) mod m via Bell triangle."""
    if N == 0:
        return []
    b = [1]  # B(0) = 1
    row = [1]
    for n in range(1, N):
        new_row = [row[-1]]
        for j in range(1, n + 1):
            new_row.append((new_row[j - 1] + row[j - 1]) % m)
        row = new_row
        b.append(row[0] % m)
    return b

def gen_cbc_mod(N, m):
    """Central binomial C(2n,n) mod m."""
    # For prime m, use Lucas' theorem or direct computation
    terms = []
    for n in range(N):
        val = binomial(2 * n, n) % m
        terms.append(val)
    return terms

def gen_fib_mod(N, m):
    """Fibonacci mod m."""
    if N == 0:
        return []
    f = [0, 1]
    for i in range(2, N):
        f.append((f[-1] + f[-2]) % m)
    return f[:N]

def gen_category_C(N=500):
    """Mod-reductions of classical sequences (~60 lines)."""
    # Partition numbers mod m
    for m in [6, 10, 12, 15, 25, 35, 49]:
        terms = gen_partition_mod(N, m)
        emit(f"SYNTH_part_mod{m}", m, terms)

    # Catalan numbers mod m
    for m in [3, 5, 7, 11, 13]:
        terms = gen_catalan_mod(min(N, 300), m)
        emit(f"SYNTH_catalan_mod{m}", m, terms)

    # Bell numbers mod m
    for m in [5, 7, 11, 13]:
        terms = gen_bell_mod(min(N, 300), m)
        emit(f"SYNTH_bell_mod{m}", m, terms)

    # Central binomial mod m
    for m in [3, 5, 7, 11]:
        terms = gen_cbc_mod(min(N, 300), m)
        emit(f"SYNTH_cbc_mod{m}", m, terms)

    # Fibonacci mod composite m
    for m in [6, 10, 12, 15, 21, 35]:
        terms = gen_fib_mod(N, m)
        emit(f"SYNTH_fib_mod{m}", m, terms)


# =========================================================================
# Category E: Stern-Brocot and digit-counting
# =========================================================================

def gen_stern(N):
    """Stern diatomic A002487."""
    a = [0] * max(N, 2)
    a[0] = 0
    a[1] = 1
    for n in range(2, N):
        if n % 2 == 0:
            a[n] = a[n // 2]
        else:
            a[n] = a[n // 2] + a[n // 2 + 1]
    return a[:N]

def gen_category_E(N=500):
    """Stern-Brocot and digit-counting (~30 lines)."""
    # Stern diatomic mod p
    stern = gen_stern(N)
    for p in [3, 5, 7, 11, 13]:
        emit(f"A002487_A{p}", p, [s % p for s in stern])

    # Bit length floor(log2(n))+1
    bit_len = [0] + [n.bit_length() for n in range(1, N)]
    for A in [2, 3, 4, 5]:
        emit(f"A070939_A{A}", A, [b % A for b in bit_len[:N]])

    # Binary weight (popcount)
    popcount = [bin(n).count('1') for n in range(N)]
    for A in [2, 3, 4, 5]:
        emit(f"A000120_A{A}", A, [p % A for p in popcount[:N]])

    # Highest power of 2 dividing n: a(n) = 2^v_2(n)
    hp2 = [0] + [2 ** v_p(n, 2) for n in range(1, N)]
    for A in [3, 4, 5, 7, 8]:
        emit(f"A006519_A{A}", A, [h % A for h in hp2[:N]])


# =========================================================================
# Categories B and F: Download from OEIS (require network)
# =========================================================================

def download_bfile(anum):
    """Download b-file from OEIS, return list of (index, value) pairs."""
    import urllib.request
    num = int(anum.replace("A", ""))
    url = f"https://oeis.org/A{num:06d}/b{num:06d}.txt"
    try:
        req = urllib.request.Request(url, headers={
            'User-Agent': 'Mozilla/5.0 (omnis research)',
            'Accept': 'text/plain'})
        with urllib.request.urlopen(req, timeout=15) as resp:
            lines = resp.read().decode('utf-8', errors='replace').splitlines()
        pairs = []
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) >= 2:
                try:
                    pairs.append((int(parts[0]), int(parts[1])))
                except ValueError:
                    continue
        return pairs
    except Exception as e:
        print(f"# WARN: failed to download {anum}: {e}", file=sys.stderr)
        return []

def bfile_to_terms(pairs, N, offset=None):
    """Convert (index, value) pairs to a list of N terms starting from the smallest index."""
    if not pairs:
        return []
    if offset is None:
        offset = pairs[0][0]
    idx_map = {p[0]: p[1] for p in pairs}
    terms = []
    for i in range(offset, offset + N):
        if i in idx_map:
            terms.append(idx_map[i])
        else:
            break
    return terms

def gen_category_B(N=500):
    """Automatic/morphic sequences from OEIS b-files (~30 lines)."""
    targets = [
        "A010060",  # Thue-Morse
        "A003849",  # Fibonacci word
        "A014577",  # Regular paperfolding
        "A005614",  # Binary complement of Fibonacci word
        "A080846",  # Cubefree binary
        "A001285",  # Thue-Morse +1
    ]
    for anum in targets:
        pairs = download_bfile(anum)
        if not pairs:
            continue
        terms = bfile_to_terms(pairs, N)
        if len(terms) < 100:
            continue
        for A in [2, 3, 5]:
            t = [abs(v) % A for v in terms[:min(N, len(terms))]]
            emit(f"{anum}_A{A}", A, t)

def gen_category_F(N=500):
    """Broad mining from OEIS b-files (~200 lines)."""
    targets = [
        "A005185", "A006577", "A003188", "A005940",
        "A030101", "A030109", "A059893", "A054429",
        "A000523", "A001511", "A065359", "A048896", "A000975",
        "A004718", "A023416", "A005811", "A030308",
        "A060833", "A080100", "A089010",
        "A006068", "A003714", "A073642", "A005536",
        "A056539", "A064413", "A083652",
    ]
    for anum in targets:
        pairs = download_bfile(anum)
        if not pairs:
            continue
        terms = bfile_to_terms(pairs, N)
        if len(terms) < 100:
            continue
        for A in [2, 3, 4, 5, 7, 8]:
            t = [abs(v) % A for v in terms[:min(N, len(terms))]]
            emit(f"{anum}_A{A}", A, t)


# =========================================================================
# Main
# =========================================================================

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 generate_candidates.py <D|C|E|B|F>", file=sys.stderr)
        sys.exit(1)

    cat = sys.argv[1].upper()
    # Replaced by updated main below


# =========================================================================
# Category G: 3-state Cellular Automata (center-cell sequences)
# =========================================================================

def gen_category_G(N=300, n_rules=500):
    """Sample 3-state 3-neighbor CA rules, generate center-cell sequences."""
    import random
    random.seed(42)  # reproducible
    
    WW = 2 * N + 100  # wide enough
    
    sampled = set()
    count = 0
    while count < n_rules:
        # Random 3-state rule: 27 entries, each 0-2
        rule = tuple(random.randint(0, 2) for _ in range(27))
        if rule in sampled:
            continue
        sampled.add(rule)
        
        # Simulate CA
        row = [0] * WW
        row[WW // 2] = 1  # single seed
        center = []
        
        for t in range(N):
            center.append(row[WW // 2])
            new_row = [0] * WW
            for i in range(1, WW - 1):
                neighborhood = row[i-1] * 9 + row[i] * 3 + row[i+1]
                new_row[i] = rule[neighborhood]
            row = new_row
        
        # Skip all-zero sequences (trivial)
        if all(v == 0 for v in center[1:]):
            continue
        
        # Rule number (base-3 encoding of the 27 entries)
        rule_num = sum(rule[i] * (3 ** i) for i in range(27))
        
        # Output at A=3 (natural alphabet)
        emit(f"CA3_{rule_num}", 3, center)
        count += 1


# =========================================================================
# Category H: OEIS "no formula" sequences
# =========================================================================

def gen_category_H(N=500, max_seqs=100):
    """Download OEIS sequences with keyword 'more' (needs more info)."""
    import urllib.request
    import json
    import time
    
    # Search OEIS for sequences with keyword "more" 
    # Use the OEIS search API
    collected = 0
    
    # Known sequences with "more" keyword or no formula
    # These are manually curated from OEIS searches
    targets = [
        # Sequences flagged as needing more terms/formulas
        "A005185",  # Alcuin's sequence
        "A006577",  # Collatz steps (no closed form)
        "A006667",  # floor(n^(3/2))
        "A003188",  # Gray code
        "A005940",  # Doudna sequence
        "A030101",  # Binary reversal
        "A030109",  # Reverse binary
        "A059893",  # Bit reverse
        "A054429",  # Natural ruler function
        "A063787",  # Binary order
        "A000523",  # floor(log2(n))
        "A001511",  # 2-ruler function
        "A065359",  # Odd part of n
        "A048896",  # Binary weight * something
        "A000975",  # Binary 10101...
        "A004718",  # Tower of Hanoi
        "A023416",  # Number of 0s in binary
        "A005811",  # Number of runs in binary
        "A060833",  # 2*n XOR n
        "A080100",  # Prouhet-Thue-Morse related
        "A006068",  # Inverse Gray code
        "A003714",  # Fibbinary numbers
        "A073642",  # Largest prime power <= n
        "A005536",  # Friedman sequence
        "A056539",  # Self-conjugate partitions
        "A064413",  # EKG sequence
        "A083652",  # 3x+1 related
        "A007306",  # Denominator of Farey
        "A002487",  # Stern diatomic (already in E, try more A values)
        "A007947",  # Squarefree part
        "A057979",  # Digital root related
        "A091297",  # Binary carry
        "A053645",  # Most significant bit removed
        "A006519",  # Highest power of 2 dividing n
        "A007913",  # Squarefree part of n
        "A008683",  # Mobius function
        "A010873",  # n mod 4
        "A035263",  # Period doubling
        "A001222",  # BigOmega
        "A008966",  # Squarefree indicator
    ]
    
    for anum in targets:
        if collected >= max_seqs:
            break
        pairs = download_bfile(anum)
        if not pairs:
            continue
        terms = bfile_to_terms(pairs, N)
        if len(terms) < 100:
            continue
        for A in [2, 3, 4, 5]:
            t = [abs(v) % A for v in terms[:min(N, len(terms))]]
            emit(f"{anum}_A{A}", A, t)
        collected += 1
    
    if collected == 0:
        print("# No sequences downloaded", file=sys.stderr)


# Update main
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 generate_candidates.py <D|C|E|B|F|G|H>", file=sys.stderr)
        sys.exit(1)

    cat = sys.argv[1].upper()
    if cat == "D":
        gen_category_D()
    elif cat == "C":
        gen_category_C()
    elif cat == "E":
        gen_category_E()
    elif cat == "B":
        gen_category_B()
    elif cat == "F":
        gen_category_F()
    elif cat == "G":
        gen_category_G()
    elif cat == "H":
        gen_category_H()
    else:
        print(f"Unknown category '{cat}'. Use D, C, E, B, F, G, or H.", file=sys.stderr)
        sys.exit(1)
