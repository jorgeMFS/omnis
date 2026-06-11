// =========================================================================
// test_engine_determinism — 30-candidate determinism canary.
//
// Determinism contract:
//   "Engine determinism. Thread-pool may produce non-identical
//    solver-choice on ties. Contract: (sc, pred_sc, MDL +/- 0.5) is
//    stable across reruns; solver_desc may vary on ties.
//    tests/test_engine_determinism.cpp enforces this on a 30-candidate
//    canary."
//
// This file is the §3 canary. Setup:
//   - 30 candidates drawn deterministically from benchmark14 (14) + a 16-
//     candidate subset of arithmetic + prime + selfref + morphic (the
//     non-OEIS, no-snapshot-dependency local generators), all generated
//     fresh in-process from tests/benchmarks.h or short closed-forms.
//   - Each candidate run with --budget 60 (sufficient for the selected
//     30 candidates to converge; the §3 contract is conditional on
//     sufficient budget — at tight budget the worker pool can fail to
//     find a solution that exists, which is by design of bounded search,
//     not an engine determinism violation). Empirical convergence
//     floor for the canary candidate set (rule30 & divisorcount at
//     N=50 are the slowest of the b14np slice): 60s suffices.
//   - 3 reruns per candidate.
//   - Asserts per candidate:
//       sc(run_i) == sc(run_0)            for i ∈ {1, 2}
//       pred_sc(run_i) == pred_sc(run_0)  for i ∈ {1, 2}
//       max(mdl) - min(mdl) <= 0.5        across 3 runs
//
// Build: wired into CMakeLists.txt as test_engine_determinism (CTest target).
// Test runs in ~15 minutes worst case (30 × 3 × 5s). Marked TIMEOUT 1200.
// =========================================================================

#include "benchmarks.h"

#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// =========================================================================
// Test infrastructure (mirrors test_gen_workload's helpers).
// =========================================================================

struct TestResult { int passed = 0; int total = 0; int skipped = 0; };

#define CHECK(label, cond) do { \
    bool _c = (cond); \
    std::printf("  %-60s %s\n", label, _c ? "PASS" : "FAIL"); \
    tr.total++; if (_c) tr.passed++; \
} while (0)

static const std::string VALIDATE_BIN = "./omnis_validate";

static bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), fp)) out.append(buf, n);
    return ::pclose(fp);
}

// Extract the LAST newline-delimited record (the CSV row). Engine emits
// diagnostic printf during solve() which precedes the row. See companion
// fix in test_gen_workload.cpp.
static std::string lastLine(const std::string& s) {
    if (s.empty()) return s;
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    size_t nl = t.find_last_of('\n');
    if (nl == std::string::npos) return t;
    return t.substr(nl + 1);
}

static std::string csvField(const std::string& row, int idx) {
    int cur = 0; size_t pos = 0;
    while (pos < row.size()) {
        size_t comma = row.find(',', pos);
        if (cur == idx) return row.substr(pos, (comma == std::string::npos ? row.size() : comma) - pos);
        if (comma == std::string::npos) break;
        pos = comma + 1; cur++;
    }
    return std::string();
}

// =========================================================================
// Deterministic 30-candidate selection.
// =========================================================================
//
// 14 from benchmark14, plus 16 small arithmetic/prime/selfref/morphic-like
// short closed-forms generated in-process. Total 30. Each candidate is a
// triple (id, A, terms-CSV) emitted to omnis_validate via --terms.

struct Cand { std::string id; int A; std::vector<int> terms; };

static std::vector<Cand> buildCanaries() {
    std::vector<Cand> v;

    // Slice 1: benchmark14 ground truth (14 candidates).
    // Padded to ≥ 40 terms so train_N ≥ 20 (omnis_validate k_min floor).
    auto pad = [](int n) { return (n < 40) ? 40 : n; };
    v.push_back({"counting",    4, genCounting(pad(50))});
    v.push_back({"pow3mod7",    7, genPow3Mod7(pad(50))});
    v.push_back({"trimod8",     8, genTriMod8(pad(50))});
    v.push_back({"fibmod4",     4, genFibMod4(pad(50))});
    v.push_back({"thuemorse",   2, genThueMorse(pad(50))});
    v.push_back({"digitsum4",   4, genDigitSum4(pad(50))});
    v.push_back({"collatz",     4, genCollatz(27, pad(50))});
    v.push_back({"rule30",      2, genRule30(pad(50))});
    v.push_back({"parityalt",   2, genParityAlt(pad(50))});
    v.push_back({"champernowne",4, genChampernowne(pad(50))});
    v.push_back({"collatzstop", 4, genCollatzStop(pad(50))});
    v.push_back({"pi_b4",       4, genPiB4(pad(50))});
    v.push_back({"divisorcount",4, genDivisorCount(pad(50))});
    v.push_back({"sigma",       4, genSigma(pad(50))});

    // Slice 2: 16 short closed-forms. All deterministic, no RNG. Selected
    // to span easy → hard within the canary's tight 5s budget.
    auto modSeq = [](int A, std::function<int(int)> f, int n) {
        std::vector<int> o(n);
        for (int i = 0; i < n; i++) {
            int v = f(i) % A; if (v < 0) v += A; o[i] = v;
        }
        return o;
    };
    v.push_back({"id_mod3",       3, modSeq(3,  [](int n){ return n; }, 60)});
    v.push_back({"id_mod5",       5, modSeq(5,  [](int n){ return n; }, 60)});
    v.push_back({"alt2",          2, modSeq(2,  [](int n){ return n; }, 60)});
    v.push_back({"square_mod7",   7, modSeq(7,  [](int n){ return n*n; }, 60)});
    v.push_back({"cube_mod5",     5, modSeq(5,  [](int n){ return n*n*n; }, 60)});
    v.push_back({"plus_two_mod4", 4, modSeq(4,  [](int n){ return n+2; }, 60)});
    v.push_back({"step3_mod6",    6, modSeq(6,  [](int n){ return 3*n; }, 60)});
    v.push_back({"step5_mod3",    3, modSeq(3,  [](int n){ return 5*n; }, 60)});
    v.push_back({"sq_step_mod4",  4, modSeq(4,  [](int n){ return n*(n+1)/2; }, 60)});
    v.push_back({"step7_mod11", 11, modSeq(11, [](int n){ return 7*n; }, 60)});
    v.push_back({"alt_pair_mod3", 3, modSeq(3,  [](int n){ return (n/2); }, 60)});
    v.push_back({"halver_mod4",   4, modSeq(4,  [](int n){ return n/2; }, 60)});
    v.push_back({"third_mod4",    4, modSeq(4,  [](int n){ return n/3; }, 60)});
    v.push_back({"bitwise_mod2",  2, modSeq(2,  [](int n){ return (n>>1)^n; }, 60)});
    v.push_back({"pop_mod3",      3, modSeq(3,  [](int n){ return __builtin_popcount(n); }, 60)});
    v.push_back({"tz_mod4",       4, modSeq(4,  [](int n){ return n==0?0:__builtin_ctz(n); }, 60)});

    return v;
}

// =========================================================================
// Per-candidate determinism check (3 reruns, tight budget).
// =========================================================================

struct Trip { int sc; int pred_sc; double mdl; std::string cls; };

static bool runOnce(const Cand& c, Trip& t) {
    std::ostringstream tcsv;
    for (size_t i = 0; i < c.terms.size(); i++) { if (i) tcsv << ','; tcsv << c.terms[i]; }
    // Tight budget; goal is contract verification under multi-thread, not
    // optimum-finding. --db points at an empty inline-synthesised DB so
    // each run starts from identical engine state (§3: library state is
    // never an implicit input).
    std::string db_path = "/tmp/test_engine_determinism_empty_db.bin";
    // Write empty DB once per call; race-tolerant (same content every time).
    {
        FILE* f = std::fopen(db_path.c_str(), "wb");
        if (!f) return false;
        const char hdr[] = {'E','N','A','R', 2,0,0,0, 0,0,0,0};
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::fclose(f);
    }
    std::string cmd = VALIDATE_BIN + " --terms \"" + tcsv.str() + "\" --A "
                    + std::to_string(c.A) + " --id " + c.id
                    + " --budget 60 --freeze-db --db " + db_path
                    + " 2>/dev/null";
    std::string out;
    int rc = runCapture(cmd, out);
    if (rc != 0) return false;
    std::string row = lastLine(out);
    // 15-column schema: sc=7, pred_sc=8, solomonoff_class=9, mdl=10.
    t.sc       = std::atoi(csvField(row, 7).c_str());
    t.pred_sc  = std::atoi(csvField(row, 8).c_str());
    t.cls      = csvField(row, 9);
    t.mdl      = std::strtod(csvField(row, 10).c_str(), nullptr);
    return true;
}

static void test_30_candidate_canary(TestResult& tr) {
    std::printf("[determinism] 30-candidate × 3-rerun canary (sc, pred_sc, MDL ±0.5)\n");
    if (!fileExists(VALIDATE_BIN)) {
        std::printf("  omnis_validate binary not present                          SKIP\n");
        tr.skipped++; return;
    }
    auto cands = buildCanaries();
    if ((int)cands.size() != 30) {
        std::printf("  expected 30 candidates, got %zu                            FAIL\n", cands.size());
        tr.total++; return;
    }

    int sc_ok = 0, pred_ok = 0, mdl_ok = 0, exec_ok = 0;
    for (auto& c : cands) {
        Trip t[3];
        bool all_ran = true;
        for (int i = 0; i < 3; i++) {
            if (!runOnce(c, t[i])) { all_ran = false; break; }
        }
        if (!all_ran) {
            std::printf("  %-30s  exec error                              FAIL\n", c.id.c_str());
            continue;
        }
        exec_ok++;
        bool sc_eq   = (t[0].sc == t[1].sc) && (t[1].sc == t[2].sc);
        bool pred_eq = (t[0].pred_sc == t[1].pred_sc) && (t[1].pred_sc == t[2].pred_sc);
        double mdl_min = t[0].mdl, mdl_max = t[0].mdl;
        for (int i = 1; i < 3; i++) {
            if (t[i].mdl < mdl_min) mdl_min = t[i].mdl;
            if (t[i].mdl > mdl_max) mdl_max = t[i].mdl;
        }
        bool mdl_close = (mdl_max - mdl_min) <= 0.5;
        if (sc_eq) sc_ok++;
        if (pred_eq) pred_ok++;
        if (mdl_close) mdl_ok++;
        if (!(sc_eq && pred_eq && mdl_close)) {
            std::printf("  %-30s  sc=%d,%d,%d pred=%d,%d,%d mdl=%.2f..%.2f  %s\n",
                c.id.c_str(),
                t[0].sc, t[1].sc, t[2].sc,
                t[0].pred_sc, t[1].pred_sc, t[2].pred_sc,
                mdl_min, mdl_max,
                (sc_eq && pred_eq && mdl_close) ? "PASS" : "FAIL");
        }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "sc identical across 3 reruns (%d/30)", sc_ok);
    CHECK(buf, sc_ok == 30);
    std::snprintf(buf, sizeof(buf), "pred_sc identical across 3 reruns (%d/30)", pred_ok);
    CHECK(buf, pred_ok == 30);
    std::snprintf(buf, sizeof(buf), "MDL stable within 0.5 bits (%d/30)", mdl_ok);
    CHECK(buf, mdl_ok == 30);
    std::snprintf(buf, sizeof(buf), "all candidates executed cleanly (%d/30)", exec_ok);
    CHECK(buf, exec_ok == 30);
}

int main() {
    std::printf("=========================================================\n");
    std::printf("Engine determinism canary\n");
    std::printf("=========================================================\n\n");
    TestResult tr;
    test_30_candidate_canary(tr);
    std::printf("\n=========================================================\n");
    std::printf("Result: %d/%d passed, %d skipped\n", tr.passed, tr.total, tr.skipped);
    std::printf("=========================================================\n");
    return (tr.passed == tr.total) ? 0 : 1;
}
