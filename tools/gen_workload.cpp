// gen_workload.cpp — Deterministic workload generator for OMNIS categorical sweep.
//
// Pure C++17. No engine link. Emits omnis-workload v1 format on stdout.
//
// Usage:
//   gen_workload --category <name> [--n N] [--seed S] [--out PATH]
//
// Categories (this file, local generators only):
//   eca256              all 256 elementary CA rules, A=2, default N=500
//   totalistic_3state   all 3^7 = 2187 totalistic 3-state 3-neighbor rules, A=3, N=500
//   collatz_grid        60 (k,c,base) generalized Collatz, mixed A, N=200
//   arithmetic          7 number-theoretic functions (d, phi, sigma, omega, ...), A=4, N=200
//   selfref             3 self-referential (Kolakoski, Recaman, BitReversal), N=200
//   prime               3 prime-related (indicator, gaps, primes mod), N=500/200
//   morphic             3 morphic sequences (Rudin-Shapiro, Baum-Sweet, period-doubling), A=2, N=200
//   neg_controls        20 negative controls (random, Pi-b4, etc.), N=500
//   benchmark14         14 hand-written reference benchmarks (12 curated targets + 2 nested-loop smoke tests)
//   all_local           emit all the above into separate sections (sequential)
//
// OEIS-sourced categories (oeis_core, oeis_easy, etc.) are produced by
// tools/oeis_loader.cpp, not this binary.
//
// Determinism: same (category, seed, n) -> byte-identical stdout.
// Stable across runs as long as GENERATOR_VERSION is unchanged.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Generator versioning
// =============================================================================
//
// Bump GENERATOR_VERSION whenever any generator's output for any (category, n,
// seed) tuple changes. This is the contract that workload-file headers refer
// to. CI / tests verify that committed workload checksums match the current
// version.
//
// Version history:
//   1  Initial version of the local generators.
//      ECA 256 + Collatz parameter grid (60) + arithmetic + selfref + prime
//      + morphic + neg_controls + benchmark14 + totalistic 3-state CA (2187).
//   2  Added DivisorCount + Sigma to benchmark; renamed benchmark12 -> benchmark14
//      to match the engine's standard 14-target benchmark list.

static constexpr int GENERATOR_VERSION = 2;
static constexpr const char* WORKLOAD_SCHEMA = "omnis-workload v1";

// =============================================================================
// Number-theoretic helpers
// =============================================================================

static int divisor_count(int n) {
    if (n <= 0) return 0;
    int d = 0;
    for (int k = 1; (int64_t)k * k <= n; k++)
        if (n % k == 0) d += (k * k == n ? 1 : 2);
    return d;
}

static int euler_phi(int n) {
    if (n <= 0) return 0;
    int r = n, m = n;
    for (int p = 2; (int64_t)p * p <= m; p++)
        if (m % p == 0) { while (m % p == 0) m /= p; r -= r / p; }
    if (m > 1) r -= r / m;
    return r;
}

static int sigma_fn(int n) {
    if (n <= 0) return 0;
    int s = 0;
    for (int k = 1; (int64_t)k * k <= n; k++)
        if (n % k == 0) { s += k; if (k * k != n) s += n / k; }
    return s;
}

static int big_omega(int n) {
    if (n <= 1) return 0;
    int c = 0;
    for (int p = 2; (int64_t)p * p <= n; p++)
        while (n % p == 0) { c++; n /= p; }
    if (n > 1) c++;
    return c;
}

static int small_omega(int n) {
    if (n <= 1) return 0;
    int c = 0;
    for (int p = 2; (int64_t)p * p <= n; p++)
        if (n % p == 0) { c++; while (n % p == 0) n /= p; }
    if (n > 1) c++;
    return c;
}

static int mobius(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    int c = 0;
    for (int p = 2; (int64_t)p * p <= n; p++) {
        if (n % (p * p) == 0) return 0;
        if (n % p == 0) { c++; n /= p; }
    }
    if (n > 1) c++;
    return (c % 2 == 0) ? 1 : -1;
}

static int liouville_lambda(int n) {
    return (big_omega(n) % 2 == 0) ? 1 : -1;
}

static std::vector<bool> sieve_of_eratosthenes(int limit) {
    std::vector<bool> is_prime(limit + 1, true);
    if (limit >= 0) is_prime[0] = false;
    if (limit >= 1) is_prime[1] = false;
    for (int i = 2; (int64_t)i * i <= limit; i++)
        if (is_prime[i])
            for (int j = i * i; j <= limit; j += i)
                is_prime[j] = false;
    return is_prime;
}

// =============================================================================
// Local sequence generators
// =============================================================================

// Random with explicit seed (default seed = 42 reproduces the baseline).
static std::vector<int> genRandom(int N, int A, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, A - 1);
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = dist(rng);
    return out;
}

// Ruler / 2-adic valuation of n+1, mod A. (A007814 mod A)
// Currently exposed only as benchmark12 component (genBmThueMorse and friends);
// kept as a free function so future categories can use it.
[[maybe_unused]] static std::vector<int> genRuler(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) {
        int n = i + 1, v = 0;
        while (n % 2 == 0) { v++; n /= 2; }
        out[i] = v % A;
    }
    return out;
}

// Elementary cellular automaton, center column, single-cell IC.
static std::vector<int> genECA(int rule, int N) {
    const int WW = 2 * N + 512;
    std::vector<int> row(WW, 0), next_row(WW, 0);
    row[WW / 2] = 1;
    std::vector<int> out;
    out.reserve(N);
    for (int t = 0; t < N; t++) {
        out.push_back(row[WW / 2]);
        for (int i = 1; i < WW - 1; i++) {
            int nb = row[i - 1] * 4 + row[i] * 2 + row[i + 1];
            next_row[i] = (rule >> nb) & 1;
        }
        std::swap(row, next_row);
        std::fill(next_row.begin(), next_row.end(), 0);
    }
    return out;
}

// Totalistic 3-state, 3-neighbor CA. The "code" encodes the rule in base 3:
// for each sum of the 3 neighbors (sum in {0..6}), the output state in {0,1,2}.
// 3^7 = 2187 distinct rules. Center column from single-cell IC (state 1 in
// middle, all 0 elsewhere).
static std::vector<int> genTotalistic3State(int code, int N) {
    const int WW = 2 * N + 512;
    std::vector<int> row(WW, 0), next_row(WW, 0);
    row[WW / 2] = 1;

    // Decode rule: lookup[sum] -> next state, sum in [0..6].
    int lookup[7];
    int c = code;
    for (int i = 0; i < 7; i++) {
        lookup[i] = c % 3;
        c /= 3;
    }

    std::vector<int> out;
    out.reserve(N);
    for (int t = 0; t < N; t++) {
        out.push_back(row[WW / 2]);
        for (int i = 1; i < WW - 1; i++) {
            int s = row[i - 1] + row[i] + row[i + 1];
            next_row[i] = lookup[s];
        }
        std::swap(row, next_row);
        std::fill(next_row.begin(), next_row.end(), 0);
    }
    return out;
}

// Generalized Collatz: x -> x/2 if even, k*x + c if odd. Output = step count
// (mod A) until x reaches 1 or 10000 steps. Diverging starts emit 0.
static std::vector<int> genCollatzGen(int k, int c, int base, int N, int off = 1) {
    std::vector<int> out;
    out.reserve(N);
    for (int n = 0; n < N; n++) {
        int64_t x = (int64_t)n + off;
        int steps = 0;
        bool diverges = false;
        for (int s = 0; s < 10000 && x > 1; s++) {
            if (x % 2 == 0) x /= 2;
            else x = (int64_t)k * x + c;
            steps++;
            if (x > (int64_t)1e15) { diverges = true; break; }
        }
        out.push_back(diverges ? 0 : steps % base);
    }
    return out;
}

static std::vector<int> genDivisorCount(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = divisor_count(i + 1) % A;
    return out;
}

static std::vector<int> genEulerPhi(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = euler_phi(i + 1) % A;
    return out;
}

static std::vector<int> genSigma(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = sigma_fn(i + 1) % A;
    return out;
}

static std::vector<int> genBigOmega(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = big_omega(i + 1) % A;
    return out;
}

static std::vector<int> genSmallOmega(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = small_omega(i + 1) % A;
    return out;
}

static std::vector<int> genMobius(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) {
        int mu = mobius(i + 1);
        out[i] = ((mu % A) + A) % A;
    }
    return out;
}

static std::vector<int> genLiouville(int N, int A) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) {
        if (A == 2) out[i] = (1 - liouville_lambda(i + 1)) / 2;
        else out[i] = big_omega(i + 1) % A;
    }
    return out;
}

// Kolakoski A000002, mapped from {1,2} to {0,1}.
static std::vector<int> genKolakoski(int N) {
    if (N <= 0) return {};
    std::vector<int> seq;
    seq.push_back(1);
    if (N == 1) {
        std::vector<int> out(1); out[0] = 0; return out;
    }
    seq.push_back(2);
    if (N == 2) {
        std::vector<int> out{0, 1}; return out;
    }
    seq.push_back(2);
    int run_idx = 2;
    while ((int)seq.size() < N) {
        int val = (seq.back() == 1) ? 2 : 1;
        int run_len = seq[run_idx];
        for (int j = 0; j < run_len && (int)seq.size() < N; j++)
            seq.push_back(val);
        run_idx++;
    }
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) out[i] = seq[i] - 1;
    return out;
}

// Recaman A005132, mod A.
static std::vector<int> genRecaman(int N, int A) {
    if (N <= 0) return {};
    std::vector<int> out(N);
    std::set<int64_t> seen;
    int64_t a = 0;
    seen.insert(0);
    out[0] = 0;
    for (int n = 1; n < N; n++) {
        int64_t cand = a - n;
        if (cand > 0 && seen.find(cand) == seen.end()) a = cand;
        else a = a + n;
        seen.insert(a);
        out[n] = (int)(((a % A) + A) % A);
    }
    return out;
}

// Bit-reversal / Van der Corput A030101, mod A.
static std::vector<int> genBitReversal(int N, int A) {
    if (N <= 0) return {};
    std::vector<int> out(N);
    out[0] = 0;
    for (int i = 1; i < N; i++) {
        int n = i, rev = 0, bits = 0;
        int temp = n;
        while (temp > 0) { bits++; temp >>= 1; }
        temp = n;
        for (int b = 0; b < bits; b++) { rev = (rev << 1) | (temp & 1); temp >>= 1; }
        out[i] = rev % A;
    }
    return out;
}

static std::vector<int> genPrimeIndicator(int N) {
    auto sieve = sieve_of_eratosthenes(N + 10);
    std::vector<int> out(N);
    for (int i = 0; i < N; i++)
        out[i] = (i + 1 < (int)sieve.size() && sieve[i + 1]) ? 1 : 0;
    return out;
}

static std::vector<int> genPrimeGaps(int N, int A) {
    auto sieve = sieve_of_eratosthenes(std::max(N * 20, 5000));
    std::vector<int> primes;
    for (int i = 2; i < (int)sieve.size(); i++)
        if (sieve[i]) primes.push_back(i);
    std::vector<int> out;
    for (int i = 1; i < (int)primes.size() && (int)out.size() < N; i++)
        out.push_back((primes[i] - primes[i - 1]) % A);
    while ((int)out.size() < N) out.push_back(0);
    return out;
}

static std::vector<int> genPrimesMod(int N, int A) {
    auto sieve = sieve_of_eratosthenes(std::max(N * 20, 5000));
    std::vector<int> primes;
    for (int i = 2; i < (int)sieve.size(); i++)
        if (sieve[i]) primes.push_back(i);
    std::vector<int> out(N);
    for (int i = 0; i < N && i < (int)primes.size(); i++) out[i] = primes[i] % A;
    return out;
}

// Rudin-Shapiro A020985: count of "11" in binary of n, mod 2.
static std::vector<int> genRudinShapiro(int N) {
    std::vector<int> out(N);
    for (int i = 0; i < N; i++) {
        int count = 0, prev = 0, n = i;
        while (n > 0) {
            int bit = n & 1;
            if (bit && prev) count++;
            prev = bit;
            n >>= 1;
        }
        out[i] = count % 2;
    }
    return out;
}

// Baum-Sweet A086747: 1 if no odd-length zero-block in binary of n, else 0.
static std::vector<int> genBaumSweet(int N) {
    if (N <= 0) return {};
    std::vector<int> out(N);
    out[0] = 1;
    for (int i = 1; i < N; i++) {
        int n = i;
        bool odd_zero = false;
        while (n > 0) {
            if (n & 1) { n >>= 1; continue; }
            int zeros = 0;
            while (n > 0 && !(n & 1)) { zeros++; n >>= 1; }
            if (zeros % 2 == 1) { odd_zero = true; break; }
        }
        out[i] = odd_zero ? 0 : 1;
    }
    return out;
}

// Period-doubling A035263: a(2n)=a(n), a(2n+1)=1-a(n), a(0)=1.
static std::vector<int> genPeriodDoubling(int N) {
    if (N <= 0) return {};
    std::vector<int> out(N);
    out[0] = 1;
    for (int i = 1; i < N; i++) {
        if (i % 2 == 0) out[i] = out[i / 2];
        else out[i] = 1 - out[(i - 1) / 2];
    }
    return out;
}

// Pi-base-4 (negative control): hardcoded hex-fractional digits, base-4 reduced.
static std::vector<int> genPiDigitsMod(int N, int A) {
    const char* hex_frac =
        "243F6A8885A308D313198A2E03707344"
        "A4093822299F31D0082EFA98EC4E6C89"
        "452821E638D01377BE5466CF34E90C6C"
        "C0AC29B7C97C50DD3F84D5B5B5470917"
        "9216D5D98979FB1BD1310BA698DFB5AC"
        "2FFD72DBD01ADFB7B8E1AFED6A267E96"
        "BA7C9045F12C7F9924A19947B3916CF7"
        "0801F2E2858EFC16636920D871574E69";
    std::vector<int> digits;
    digits.push_back(3);
    for (int i = 0; hex_frac[i] && (int)digits.size() < N * 2; i++) {
        char c = hex_frac[i];
        int v = (c >= '0' && c <= '9') ? (c - '0') :
                (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : (c - 'a' + 10);
        digits.push_back(v >> 2);
        digits.push_back(v & 3);
    }
    std::vector<int> out(N);
    for (int i = 0; i < N && i < (int)digits.size(); i++) out[i] = digits[i] % A;
    return out;
}

// Benchmark 12 — hand-written reference set. Each maps to a specific
// closed-form known to the engine (see tests/benchmarks.h).
static std::vector<int> genBmCounting(int N) { std::vector<int> o(N); for(int i=0;i<N;i++) o[i]=i%4; return o; }
static std::vector<int> genBmPow3Mod7(int N) { std::vector<int> o; int x=1; for(int i=0;i<N;i++){o.push_back(x); x=(x*3)%7;} return o; }
static std::vector<int> genBmTriMod8(int N)  { std::vector<int> o; int64_t t=0; for(int i=0;i<N;i++){o.push_back((int)(((t%8)+8)%8)); t+=(i+1);} return o; }
static std::vector<int> genBmFibMod4(int N)  { std::vector<int> o; int64_t a=0,b=1; for(int i=0;i<N;i++){o.push_back((int)(((b%4)+4)%4)); int64_t c=a+b; a=b; b=c;} return o; }
static std::vector<int> genBmThueMorse(int N){ std::vector<int> o; for(int i=0;i<N;i++) o.push_back(__builtin_popcount((unsigned)i)%2); return o; }
static std::vector<int> genBmDigitSum4(int N){ std::vector<int> o; for(int i=0;i<N;i++){int s=0,x=i; while(x>0){s+=x%4; x/=4;} o.push_back(s%4);} return o; }
static std::vector<int> genBmCollatz(int N)  { std::vector<int> o; int64_t x=1; for(int i=0;i<N;i++){o.push_back((int)(((x%4)+4)%4)); if(x%2==0) x/=2; else x=3*x+1;} return o; }
static std::vector<int> genBmRule30(int N)   { return genECA(30, N); }
static std::vector<int> genBmParityAlt(int N){ std::vector<int> o; for(int i=0;i<N;i++) o.push_back((i%2==0) ? __builtin_popcount((unsigned)i)%2 : 1 - __builtin_popcount((unsigned)i)%2); return o; }

// Champernowne-base-4: concatenation 0,1,2,3,10,11,...
static std::vector<int> genBmChampernowne(int N) {
    std::vector<int> o; o.reserve(N);
    int n = 0;
    while ((int)o.size() < N) {
        std::vector<int> dig;
        int x = n;
        if (x == 0) dig.push_back(0);
        else { while (x > 0) { dig.push_back(x % 4); x /= 4; } std::reverse(dig.begin(), dig.end()); }
        for (int d : dig) { if ((int)o.size() < N) o.push_back(d); }
        n++;
    }
    return o;
}

// Collatz stopping time mod 4, for inputs n=2,3,4,...
// Starts at x=2 (not x=1) to match the engine STEP semantics: step_count_accel
// always executes one body step at s=0 before checking R[0]<=halt, so the
// shortest Collatz sequence is "1 step from x=2 to halt 1". Starting at x=1
// would require steps=0 which the engine cannot produce. Aligns with
// src/benchmarks.h::genCollatzStop and the sieve test fixture.
static std::vector<int> genBmCollatzStop(int N) {
    std::vector<int> o; o.reserve(N);
    for (int i = 0; i < N; i++) {
        int64_t x = i + 2;
        int s = 0;
        while (x > 1 && s < 10000) {
            if (x % 2 == 0) x /= 2; else x = 3 * x + 1;
            s++;
        }
        o.push_back(s % 4);
    }
    return o;
}

// Pi-base-4 (uses same hex source as genPiDigitsMod).
static std::vector<int> genBmPiB4(int N) { return genPiDigitsMod(N, 4); }

// Smoke tests — number-theoretic functions requiring nested loops (trial
// division). Per the standard benchmark list, these expose
// engine ability to discover NESTED_LOOP shapes. DivisorCount = OEIS A000005,
// Sigma = OEIS A000203. Both use mod A=4 by convention.
static std::vector<int> genBmDivisorCount(int N) { return genDivisorCount(N, 4); }
static std::vector<int> genBmSigma(int N)        { return genSigma(N, 4); }

// =============================================================================
// SHA-256 (small portable impl, used only for body checksum in workload header)
// =============================================================================

namespace sha256_impl {
    using u32 = uint32_t;
    using u64 = uint64_t;
    static const u32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    static inline u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

    static std::string hex(const std::string& msg) {
        u32 h[8] = {
            0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
        };
        u64 bitlen = (u64)msg.size() * 8;
        std::string m = msg;
        m.push_back((char)0x80);
        while (m.size() % 64 != 56) m.push_back(0);
        for (int i = 7; i >= 0; i--) m.push_back((char)((bitlen >> (i * 8)) & 0xff));

        for (size_t off = 0; off < m.size(); off += 64) {
            u32 w[64];
            for (int i = 0; i < 16; i++) {
                w[i] = ((u32)(uint8_t)m[off + 4*i] << 24)
                     | ((u32)(uint8_t)m[off + 4*i + 1] << 16)
                     | ((u32)(uint8_t)m[off + 4*i + 2] << 8)
                     | ((u32)(uint8_t)m[off + 4*i + 3]);
            }
            for (int i = 16; i < 64; i++) {
                u32 s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
                u32 s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
            for (int i = 0; i < 64; i++) {
                u32 S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
                u32 ch = (e & f) ^ ((~e) & g);
                u32 t1 = hh + S1 + ch + K[i] + w[i];
                u32 S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
                u32 mj = (a & b) ^ (a & c) ^ (b & c);
                u32 t2 = S0 + mj;
                hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
        }
        char buf[65];
        for (int i = 0; i < 8; i++) std::snprintf(buf + i*8, 9, "%08x", h[i]);
        buf[64] = 0;
        return std::string(buf);
    }
}

// =============================================================================
// Emit
// =============================================================================

struct Emit {
    std::string id;
    int A;
    std::vector<int> terms;
};

// Format a single workload line: "<id> <A> <N> <t0> <t1> ..."
static std::string formatLine(const Emit& e) {
    std::ostringstream oss;
    oss << e.id << ' ' << e.A << ' ' << e.terms.size();
    for (int t : e.terms) oss << ' ' << t;
    return oss.str();
}

// ISO-8601 UTC. Honors SOURCE_DATE_EPOCH for reproducibility.
static std::string isoDateUtc() {
    const char* sde = std::getenv("SOURCE_DATE_EPOCH");
    time_t now = sde ? (time_t)std::strtoll(sde, nullptr, 10) : std::time(nullptr);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// Write an entire category to the given output stream:
//   1. Construct all emissions
//   2. Compute body sha256 (joined lines + '\n')
//   3. Write header + body
static int emitCategory(std::ostream& out,
                        const std::string& category,
                        const std::string& selection_rule,
                        const std::vector<Emit>& items)
{
    std::ostringstream body_oss;
    for (const auto& e : items) body_oss << formatLine(e) << '\n';
    std::string body = body_oss.str();
    std::string body_sha = sha256_impl::hex(body);

    out << "# " << WORKLOAD_SCHEMA << '\n';
    out << "# category: " << category << '\n';
    out << "# generator_version: " << GENERATOR_VERSION << '\n';
    out << "# omnis_min_version: 0.1.0\n";
    out << "# created_utc: " << isoDateUtc() << '\n';
    out << "# count: " << items.size() << '\n';
    out << "# body_sha256: " << body_sha << '\n';
    out << "# selection_rule: " << selection_rule << '\n';
    out << body;
    return (int)items.size();
}

// =============================================================================
// Category dispatch
// =============================================================================

static std::vector<Emit> buildEca256(int N) {
    std::vector<Emit> v;
    for (int rule = 0; rule < 256; rule++) {
        char id[32];
        std::snprintf(id, sizeof(id), "eca_%03d", rule);
        v.push_back({id, 2, genECA(rule, N)});
    }
    return v;
}

static std::vector<Emit> buildTotalistic3State(int N) {
    std::vector<Emit> v;
    const int total = 3 * 3 * 3 * 3 * 3 * 3 * 3; // 3^7 = 2187
    for (int code = 0; code < total; code++) {
        char id[32];
        std::snprintf(id, sizeof(id), "tot3_%04d", code);
        v.push_back({id, 3, genTotalistic3State(code, N)});
    }
    return v;
}

static std::vector<Emit> buildCollatzGrid(int N) {
    std::vector<Emit> v;
    const int ks[]   = {3, 5, 7, 9, 11};
    const int cs[]   = {1, 3, 5, 7};
    const int bases[] = {2, 3, 4};
    for (int k : ks) for (int c : cs) for (int base : bases) {
        char id[64];
        std::snprintf(id, sizeof(id), "collatz_k%d_c%d_b%d", k, c, base);
        v.push_back({id, base, genCollatzGen(k, c, base, N)});
    }
    return v;
}

static std::vector<Emit> buildArithmetic(int N) {
    std::vector<Emit> v;
    int A = 4;
    v.push_back({"arith_d_n",      A, genDivisorCount(N, A)});  // A000005
    v.push_back({"arith_phi_n",    A, genEulerPhi(N, A)});      // A000010
    v.push_back({"arith_sigma_n",  A, genSigma(N, A)});         // A000203
    v.push_back({"arith_bigomega", A, genBigOmega(N, A)});      // A001222
    v.push_back({"arith_smomega",  A, genSmallOmega(N, A)});    // A001221
    v.push_back({"arith_mobius",   A, genMobius(N, A)});        // A008683
    v.push_back({"arith_liouv",    2, genLiouville(N, 2)});     // A008836 (binary)
    return v;
}

static std::vector<Emit> buildSelfref(int N) {
    std::vector<Emit> v;
    v.push_back({"selfref_kolakoski", 2, genKolakoski(N)});     // A000002
    v.push_back({"selfref_recaman",   4, genRecaman(N, 4)});    // A005132 mod 4
    v.push_back({"selfref_bitrev",    4, genBitReversal(N, 4)});// A030101 mod 4
    return v;
}

static std::vector<Emit> buildPrime(int N) {
    // All three at the supplied N. Prime indicator at small N is sparse (mostly
    // zero) but that is the caller's choice; we honor --n strictly.
    std::vector<Emit> v;
    v.push_back({"prime_indicator", 2, genPrimeIndicator(N)});  // A010051
    v.push_back({"prime_gaps",      4, genPrimeGaps(N, 4)});    // A001223 mod 4
    v.push_back({"prime_mod",       4, genPrimesMod(N, 4)});    // A000040 mod 4
    return v;
}

static std::vector<Emit> buildMorphic(int N) {
    std::vector<Emit> v;
    v.push_back({"morphic_rudshap", 2, genRudinShapiro(N)});  // A020985
    v.push_back({"morphic_baum",    2, genBaumSweet(N)});     // A086747
    v.push_back({"morphic_perdoub", 2, genPeriodDoubling(N)});// A035263
    return v;
}

static std::vector<Emit> buildNegControls(int N, uint32_t seed) {
    std::vector<Emit> v;
    // Pi-base-4 — proven hard at the engine's ISA (BBP spigot exceeds int64
    // saturation at N >= 20; consistent with the coupling-barrier analysis).
    v.push_back({"neg_pi_b4", 4, genPiDigitsMod(N, 4)});

    // Random sequences across alphabets and seeds, expected incompressible.
    // 5 per alphabet in {2, 4} = 10 candidates.
    for (int A : {2, 4}) {
        for (int j = 0; j < 5; j++) {
            uint32_t s = seed ^ ((uint32_t)A * 1009u) ^ ((uint32_t)j * 9173u);
            char id[64];
            std::snprintf(id, sizeof(id), "neg_random_a%d_s%u", A, s);
            v.push_back({id, A, genRandom(N, A, s)});
        }
    }

    // "Crypto-style" — random with an additional XOR mask. Still random; a
    // labeled variant for the negative-control band. 5 at A=2, 4 at A=4 = 9
    // candidates. Total: 1 + 10 + 9 = 20.
    {
        int A = 2;
        for (int j = 0; j < 5; j++) {
            uint32_t s = (seed * 31u + 0x9e3779b9u) ^ ((uint32_t)A * 0xdeadbeefu) ^ ((uint32_t)j * 0xb5297a4du);
            std::vector<int> r = genRandom(N, A, s);
            for (int& x : r) x = (x ^ ((s >> (j*3)) & (A - 1))) % A;
            char id[64];
            std::snprintf(id, sizeof(id), "neg_crypto_a%d_s%u", A, s);
            v.push_back({id, A, std::move(r)});
        }
    }
    {
        int A = 4;
        for (int j = 0; j < 4; j++) {
            uint32_t s = (seed * 31u + 0x9e3779b9u) ^ ((uint32_t)A * 0xdeadbeefu) ^ ((uint32_t)j * 0xb5297a4du);
            std::vector<int> r = genRandom(N, A, s);
            for (int& x : r) x = (x ^ ((s >> (j*3)) & (A - 1))) % A;
            char id[64];
            std::snprintf(id, sizeof(id), "neg_crypto_a%d_s%u", A, s);
            v.push_back({id, A, std::move(r)});
        }
    }
    return v;
}

static std::vector<Emit> buildBenchmark14() {
    // The 14 standard benchmarks:
    // 12 "Table 1" reference programs PLUS DivisorCount + Sigma. The latter
    // two are smoke tests for nested-loop discovery — they expose engine
    // ability to find OEIS A000005 / A000203 shapes (trial-division loops).
    //
    // Each benchmark emits (published_N + K_at_published_N) terms total. The
    // validator's split K = max(20, total_n/4) reserves K terms as held-out,
    // leaving exactly published_N for training. This matches the train sizes
    // the original engine run used to discover the published programs.
    auto pad = [](int pub) {
        int total = pub + 20;
        for (int i = 0; i < 5; i++) {
            int k = std::max(20, total / 4);
            int train = total - k;
            if (train >= pub) return total;
            total += (pub - train);
        }
        return total;
    };
    std::vector<Emit> v;
    v.push_back({"bench_counting",      4, genBmCounting(    pad(200))});
    v.push_back({"bench_pow3mod7",      7, genBmPow3Mod7(    pad(50))});
    v.push_back({"bench_trimod8",       8, genBmTriMod8(     pad(50))});
    v.push_back({"bench_fibmod4",       4, genBmFibMod4(     pad(50))});
    v.push_back({"bench_thuemorse",     2, genBmThueMorse(   pad(256))});
    v.push_back({"bench_digitsum4",     4, genBmDigitSum4(   pad(100))});
    v.push_back({"bench_collatz",       4, genBmCollatz(     pad(50))});
    v.push_back({"bench_rule30",        2, genBmRule30(      pad(200))});
    v.push_back({"bench_parityalt",     2, genBmParityAlt(   pad(50))});
    v.push_back({"bench_champernowne",  4, genBmChampernowne(pad(100))});
    v.push_back({"bench_collatzstop",   4, genBmCollatzStop( pad(50))});
    v.push_back({"bench_pib4",          4, genBmPiB4(        pad(200))});
    // Smoke tests — standard benchmark definitions.
    v.push_back({"bench_divisorcount",  4, genBmDivisorCount(pad(50))});
    v.push_back({"bench_sigma",         4, genBmSigma(       pad(50))});
    return v;
}

// =============================================================================
// Selection rule strings (recorded verbatim in each workload header)
// =============================================================================

static const char* RULE_ECA256             = "enumerate rule in [0,256); A=2; single_cell_ic; center_column";
static const char* RULE_TOT3STATE          = "enumerate totalistic 3-state 3-neighbor rules in [0, 3^7); A=3; single_cell_ic; center_column";
static const char* RULE_COLLATZ_GRID       = "(k,c,base) in {3,5,7,9,11} x {1,3,5,7} x {2,3,4}; off=1; trajectory step count mod base";
static const char* RULE_ARITHMETIC         = "OEIS A000005, A000010, A000203, A001222, A001221, A008683, A008836; mod A=4 except liouville at A=2";
static const char* RULE_SELFREF            = "Kolakoski A000002 (binary); Recaman A005132 mod 4; bit-reversal A030101 mod 4";
static const char* RULE_PRIME              = "prime indicator A010051; prime gaps A001223 mod 4; primes A000040 mod 4";
static const char* RULE_MORPHIC            = "Rudin-Shapiro A020985; Baum-Sweet A086747; period-doubling A035263; all binary";
static const char* RULE_NEG_CONTROLS       = "Pi-base-4 (1) + seeded random (10, 5 per A in {2,4}) + seeded crypto-style (9, 5@A=2 + 4@A=4); all expected incompressible";
static const char* RULE_BENCHMARK14        = "12 hand-written reference targets (see tests/benchmarks.h)";

// =============================================================================
// CLI
// =============================================================================

static void printUsage() {
    std::fprintf(stderr,
        "gen_workload — deterministic workload generator for OMNIS categorical sweeps.\n"
        "\n"
        "Usage:\n"
        "  gen_workload --category <name> [--n N] [--seed S] [--out PATH]\n"
        "\n"
        "Categories:\n"
        "  eca256              all 256 ECA rules (default N=500, A=2)\n"
        "  totalistic_3state   all 2187 totalistic 3-state CAs (default N=500, A=3)\n"
        "  collatz_grid        60 generalized Collatz (default N=200)\n"
        "  arithmetic          7 number-theoretic functions (default N=200)\n"
        "  selfref             3 self-referential (default N=200)\n"
        "  prime               3 prime-related (default N=200; indicator forced to >=500)\n"
        "  morphic             3 morphic sequences (default N=200)\n"
        "  neg_controls        20 negative controls (default N=500)\n"
        "  benchmark12         12 hand-written benchmark targets\n"
        "  all_local           emit all of the above as one stream\n"
        "\n"
        "Options:\n"
        "  --n N            override default sequence length\n"
        "  --seed S         seed for stochastic categories (default 42)\n"
        "  --out PATH       write to file instead of stdout\n"
        
        "\n"
        "Determinism: same (category, n, seed) -> byte-identical stdout.\n"
    );
}

int main(int argc, char* argv[]) {
    std::string category;
    int n = -1;
    uint32_t seed = 42;
    std::string out_path;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") { printUsage(); return 0; }
        else if (a == "--category" && i + 1 < argc) category = argv[++i];
        else if (a == "--n"        && i + 1 < argc) {
            n = std::atoi(argv[++i]);
            if (n <= 0) {
                std::fprintf(stderr, "gen_workload: --n must be positive (got %d)\n", n);
                return 2;
            }
        }
        else if (a == "--seed"     && i + 1 < argc) seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--out"      && i + 1 < argc) out_path = argv[++i];
        else { std::fprintf(stderr, "gen_workload: unknown option '%s'\n", a.c_str()); printUsage(); return 2; }
    }

    if (category.empty()) {
        std::fprintf(stderr, "gen_workload: --category is required\n");
        printUsage();
        return 2;
    }

    std::ofstream ofs;
    std::ostream* out = &std::cout;
    if (!out_path.empty()) {
        ofs.open(out_path);
        if (!ofs.is_open()) { std::fprintf(stderr, "gen_workload: cannot write '%s'\n", out_path.c_str()); return 3; }
        out = &ofs;
    }

    auto pickN = [&](int default_n) { return (n > 0) ? n : default_n; };

    int total = 0;
    if (category == "eca256") {
        total = emitCategory(*out, "eca256", RULE_ECA256, buildEca256(pickN(500)));
    } else if (category == "totalistic_3state") {
        total = emitCategory(*out, "totalistic_3state", RULE_TOT3STATE, buildTotalistic3State(pickN(500)));
    } else if (category == "collatz_grid") {
        total = emitCategory(*out, "collatz_grid", RULE_COLLATZ_GRID, buildCollatzGrid(pickN(200)));
    } else if (category == "arithmetic") {
        total = emitCategory(*out, "arithmetic", RULE_ARITHMETIC, buildArithmetic(pickN(200)));
    } else if (category == "selfref") {
        total = emitCategory(*out, "selfref", RULE_SELFREF, buildSelfref(pickN(200)));
    } else if (category == "prime") {
        total = emitCategory(*out, "prime", RULE_PRIME, buildPrime(pickN(200)));
    } else if (category == "morphic") {
        total = emitCategory(*out, "morphic", RULE_MORPHIC, buildMorphic(pickN(200)));
    } else if (category == "neg_controls") {
        total = emitCategory(*out, "neg_controls", RULE_NEG_CONTROLS, buildNegControls(pickN(500), seed));
    } else if (category == "benchmark14") {
        total = emitCategory(*out, "benchmark14", RULE_BENCHMARK14, buildBenchmark14());
    } else if (category == "all_local") {
        total += emitCategory(*out, "eca256",            RULE_ECA256,        buildEca256(pickN(500)));
        total += emitCategory(*out, "totalistic_3state", RULE_TOT3STATE,     buildTotalistic3State(pickN(500)));
        total += emitCategory(*out, "collatz_grid",      RULE_COLLATZ_GRID,  buildCollatzGrid(pickN(200)));
        total += emitCategory(*out, "arithmetic",        RULE_ARITHMETIC,    buildArithmetic(pickN(200)));
        total += emitCategory(*out, "selfref",           RULE_SELFREF,       buildSelfref(pickN(200)));
        total += emitCategory(*out, "prime",             RULE_PRIME,         buildPrime(pickN(200)));
        total += emitCategory(*out, "morphic",           RULE_MORPHIC,       buildMorphic(pickN(200)));
        total += emitCategory(*out, "neg_controls",      RULE_NEG_CONTROLS,  buildNegControls(pickN(500), seed));
        total += emitCategory(*out, "benchmark14",       RULE_BENCHMARK14,   buildBenchmark14());
    } else {
        std::fprintf(stderr, "gen_workload: unknown category '%s'\n", category.c_str());
        printUsage();
        return 2;
    }

    std::fprintf(stderr, "gen_workload: emitted %d candidates for category '%s'\n",
                 total, category.c_str());
    return 0;
}
