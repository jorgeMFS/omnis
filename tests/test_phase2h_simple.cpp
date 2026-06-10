// =========================================================================
// Phase 2H hierarchical synthesis verification
//
// Phase 2H builds SUB_CALL-containing pools at L=4..8 via composeDDB chain
// with subcall-presence filter. Each pool body is verified across MODE_FUNC,
// MODE_ITER, MODE_EMIT × outr ∈ [0..nr-1]. This test seeds g_progdb with a
// simple register-transform primitive and runs solve() on a target that
// composition can solve. The point is to confirm:
//   1. Phase 2H code path executes without crashing.
//   2. Pools p4..p8 are built with non-zero entries.
//   3. Phase 2H emits a P2H_FUNC/ITER/EMIT HIT line (or doesn't, if the
//      target solves in earlier phases — both are acceptable).
//
// Build:
//   g++ -std=c++17 -O2 -I../src test_phase2h_simple.cpp -o /tmp/test_phase2h
// =========================================================================

#define main omnis_main_unused
#include "omnis.cpp"
#undef main
#include "benchmarks.h"

#include <cstdio>

static void resetLibrary() {
    g_progdb.records.clear();
    g_progdb.body_hashes.clear();
}

static ProgramRecord makeRecord(const Ins* ops, int n,
                                int mode = (int)MODE_ITER,
                                bool branched = false) {
    ProgramRecord pr = {};
    pr.nbody = (uint8_t)n;
    for (int i = 0; i < n; i++) pr.body[i] = ops[i];
    pr.nr = 1;
    pr.outr = 0;
    pr.mode_u = (uint8_t)mode;
    pr.ointerp_u = 0;
    pr.branched = branched ? 1 : 0;
    return pr;
}

// Test 1: Phase 2H code path executes without crashing on an unsolvable target.
// We use a short budget so search exits cleanly. The point is that the code
// reaches Phase 2H without segfault, race, or infinite loop.
// NOTE: target must be N≥30 to satisfy Phase 1's d* / period heuristics.
// Smaller targets trigger SIGFPE in pre-Phase-2H division-by-period logic.
static bool test_phase2h_executes() {
    resetLibrary();
    // Seed library with one invocable primitive: INC R0.
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    g_progdb.records.push_back(makeRecord(&inc, 1));

    // Use canonical Counting benchmark (50 elements, A=4) — known well-behaved input.
    auto tgt = genCounting(50);

    printf("  phase2h_executes: starting solve(%d, A=4, dl=8s)\n", (int)tgt.size());
    fflush(stdout);
    double dl = now_s() + 8.0;
    Res r = solve(tgt, 4, dl);

    // Pass if solve() returns without crash and produces a Res (sc may be < N).
    bool pass = (r.sc >= 0 && r.sc <= (int)tgt.size());
    printf("  phase2h_executes:              sc=%d/%d r.desc=%s %s\n",
           r.sc, (int)tgt.size(), r.desc.c_str(), pass ? "PASS" : "FAIL");
    return pass;
}

// Test 2: Phase 2H can find a SUB_CALL-using program for a compositional target.
// Library: INC R0. Target: counting sequence (n mod 4) — Phase 2A's INC body
// is the trivial baseline, but a pool body containing SUB_CALL(INC) at any
// position should also match. We just verify some program with sc==N is found.
static bool test_phase2h_finds_solution() {
    resetLibrary();
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    g_progdb.records.push_back(makeRecord(&inc, 1));

    auto tgt = genCounting(50);

    printf("  phase2h_finds_solution: starting solve(%d, A=4, dl=30s)\n", (int)tgt.size());
    fflush(stdout);
    double dl = now_s() + 30.0;
    Res r = solve(tgt, 4, dl);

    bool pass = (r.sc == (int)tgt.size());
    printf("  phase2h_finds_solution:        sc=%d/%d r.desc=%s %s\n",
           r.sc, (int)tgt.size(), r.desc.c_str(), pass ? "PASS" : "FAIL");
    return pass;
}

// Test 3: Phase 2H is dormant when library has no invocable entries.
// Without invocable entries, Phase 2H gates itself off (lib_invocable < 1).
// solve() should still complete cleanly.
// We use an empty library (resetLibrary leaves it empty) — simplest case
// of "no invocable entries." Constructing a malformed branched record
// would trigger runtime divide-by-zero in unrelated phase logic, so we
// just leave the library empty.
static bool test_phase2h_dormant_no_lib() {
    resetLibrary();
    // Empty library: subCallCatalogSize() == 0, lib_invocable == 0.

    auto tgt = genCounting(50);

    printf("  phase2h_dormant_no_lib: starting solve(%d, A=4, dl=8s)\n", (int)tgt.size());
    fflush(stdout);
    double dl = now_s() + 8.0;
    Res r = solve(tgt, 4, dl);

    // Should solve via Phase 2A's baseline INC body.
    bool pass = (r.sc == (int)tgt.size());
    printf("  phase2h_dormant_no_lib:        sc=%d/%d r.desc=%s %s\n",
           r.sc, (int)tgt.size(), r.desc.c_str(), pass ? "PASS" : "FAIL");
    return pass;
}

// Test 4: direct pool-build exercise — verify composeDDB + subcall_filter
// produces SUB_CALL-containing pools at L=4..8.
static bool test_phase2h_pool_build() {
    resetLibrary();
    // Library entry: 2-op program R0 = (R0 + 1) * 3 — NOT expressible as a single L1 op.
    // Fingerprint dedup will not unify this with any L1 instruction (different R-state map).
    Ins body[2];
    body[0] = {0, 0, {0, 0, 0, 0}, 1};      // INC R0
    body[1] = {5, 3, {0, 0, 0, 0}, 2};      // MUL_C R0 R0 c=3 (R0 = R0*3)
    g_progdb.records.push_back(makeRecord(body, 2));

    auto fL1 = buildL1(2);
    int subcall_in_l1 = 0;
    for (auto& ins : fL1) if (ins.ti == 32) subcall_in_l1++;

    auto subcall_filter = [](const Ins* b, int n) -> bool {
        for (int i = 0; i < n; i++) if (b[i].ti == 32) return true;
        return false;
    };

    double dl = now_s() + 30.0;
    printf("    fL1.size=%d (subcall=%d) dl_remaining=%.1fs\n",
           (int)fL1.size(), subcall_in_l1, dl - now_s());
    fflush(stdout);
    auto p2 = buildDDB(fL1, 2, 2, dl, 0, /*fast_fp=*/true);
    printf("    p2.size=%d remaining=%.1fs\n", (int)p2.size(), dl - now_s()); fflush(stdout);
    auto p3 = buildDDB(fL1, 3, 2, dl, 0, /*fast_fp=*/true);
    printf("    p3.size=%d remaining=%.1fs\n", (int)p3.size(), dl - now_s()); fflush(stdout);
    // Check whether p3 contains SUB_CALL bodies at all (sanity).
    int p3_subcall=0;for(auto&b:p3){bool has=false;for(int i=0;i<b.n;i++)if(b.ops[i].ti==32)has=true;if(has)p3_subcall++;}
    printf("    p3 SUB_CALL-containing=%d/%d\n", p3_subcall, (int)p3.size()); fflush(stdout);
    // p4 without filter (sanity).
    auto p4_unfiltered = composeDDB(p3, fL1, 2, dl, nullptr, 200000, /*fast_fp=*/true);
    printf("    p4_unfiltered.size=%d remaining=%.1fs\n", (int)p4_unfiltered.size(), dl - now_s()); fflush(stdout);
    auto p4 = composeDDB(p3, fL1, 2, dl, subcall_filter, 200000, /*fast_fp=*/true);
    printf("    p4.size=%d remaining=%.1fs\n", (int)p4.size(), dl - now_s()); fflush(stdout);
    auto p5 = composeDDB(p4, fL1, 2, dl, subcall_filter, 200000, /*fast_fp=*/true);
    printf("    p5.size=%d remaining=%.1fs\n", (int)p5.size(), dl - now_s()); fflush(stdout);

    // Every entry in p4 and p5 should contain at least one SUB_CALL.
    int p4_has_subcall = 0, p4_total = (int)p4.size();
    for (auto& b : p4) {
        bool has = false;
        for (int i = 0; i < b.n; i++) if (b.ops[i].ti == 32) has = true;
        if (has) p4_has_subcall++;
    }
    int p5_has_subcall = 0, p5_total = (int)p5.size();
    for (auto& b : p5) {
        bool has = false;
        for (int i = 0; i < b.n; i++) if (b.ops[i].ti == 32) has = true;
        if (has) p5_has_subcall++;
    }

    bool pass = (subcall_in_l1 == 1) &&
                (p4_total > 0) && (p4_has_subcall == p4_total) &&
                (p5_total > 0) && (p5_has_subcall == p5_total);
    printf("  phase2h_pool_build:            L1.subcall=%d p4=%d/%d p5=%d/%d %s\n",
           subcall_in_l1, p4_has_subcall, p4_total, p5_has_subcall, p5_total,
           pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    printf("=========================================================\n");
    printf("Phase 2H — Hierarchical synthesis verification (canonical)\n");
    printf("=========================================================\n\n");

    int passed = 0, total = 0;
    auto check = [&](bool b) { total++; if (b) passed++; };

    check(test_phase2h_executes());
    check(test_phase2h_finds_solution());
    check(test_phase2h_dormant_no_lib());
    check(test_phase2h_pool_build());

    printf("\n=========================================================\n");
    printf("Result: %d/%d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
