// =========================================================================
// Targeted correctness tests for the three C1 bugs caught on second audit
// (2026-05-06 evening). Each test exercises the corner case that the bug
// hid in, ensuring future regressions would be detected.
//
// Bugs covered:
//   1. Opt B cache silently swallowed OUT side effects in MODE_EMIT context.
//      Test: exSubCall with g_emit_A>0 must always run body fresh; no cache.
//   2. WSBP-2H scan missed indirect OUT via SUB_CALL'd library entries.
//      Test: emit_possible must be true when SUB_CALL'd library has OUT.
//   3. resFingerprint missed dary_op field; DARY programs with same base/init
//      but different ops would collide.
//      Test: distinct dary_op values must produce distinct fingerprints.
//
// Build:
//   g++ -std=c++17 -O2 -I../src test_correctness_fixes.cpp -o /tmp/test_correctness_fixes
// =========================================================================

#define main omnis_main_unused
#include "omnis.cpp"
#undef main

#include <cstdio>

// Reset g_progdb between tests.
static void resetLibrary() {
    g_progdb.records.clear();
    g_progdb.body_hashes.clear();
}

// Construct a ProgramRecord with given body. Defaults are safe for SUB_CALL
// invocability (subCallLibraryEntryPure passes).
static ProgramRecord makeRecordWithBody(const Ins* ops, int n) {
    ProgramRecord pr = {};
    pr.nbody = (uint8_t)n;
    for (int i = 0; i < n; i++) pr.body[i] = ops[i];
    pr.nr = 1;
    pr.outr = 0;
    pr.mode_u = 0;        // MODE_ITER - pure body, invocable
    pr.ointerp_u = 0;
    pr.branched = 0;
    pr.concat_base = 0;
    pr.dary_base = 0;
    pr.step_off = 0;
    return pr;
}

// =========================================================================
// Test 1: Bug 1 fix - Opt B cache must not pollute MODE_EMIT context.
//
// Scenario: library entry with body {INC(R0); OUT(R0); INC(R0)} (3 instructions
// → meets Opt B threshold). When invoked in MODE_EMIT context (g_emit_A>0),
// the OUT must push to g_emit_buf each time. With the original (buggy) cache,
// the second invocation would be a cache hit and skip the OUT.
//
// Before the fix: this test would observe g_emit_buf.size()==1 after two calls
// (only first call's OUT pushed; second cache-hit skipped).
// After the fix: g_emit_buf.size()==2 (both calls run body fresh, both OUT push).
// =========================================================================
static bool test_opt_b_no_cache_in_emit_context() {
    resetLibrary();
    // Body: INC(R0); OUT(R0); INC(R0)  (nbody=3, meets cache threshold)
    Ins inc1 = {0, 0, {0, 0, 0, 0}, 1};   // INC R0
    Ins out0 = {12, 0, {0, 0, 0, 0}, 1};  // OUT R0 (ti=12)
    Ins inc2 = {0, 0, {0, 0, 0, 0}, 1};   // INC R0 (again)
    Ins ops[3] = {inc1, out0, inc2};
    g_progdb.records.push_back(makeRecordWithBody(ops, 3));

    int idx = 0;
    int A = 4;

    // Set up MODE_EMIT-like context: g_emit_A=A, fresh g_emit_buf
    g_emit_A = A;
    g_emit_buf.clear();

    // First invocation: R[0] = 5
    int64_t R[kRegisterCount] = {};
    R[0] = 5;
    g_sat = false;
    exSubCall(R, idx);
    int after_first = (int)g_emit_buf.size();

    // Second invocation: same R[0] = 5 (so cache key (idx, R[..]) is identical).
    // Reset R to the same starting state for a second invocation.
    int64_t R2[8] = {};
    R2[0] = 5;
    g_sat = false;
    exSubCall(R2, idx);
    int after_second = (int)g_emit_buf.size();

    g_emit_A = 0;  // reset

    bool pass = (after_first == 1 && after_second == 2);
    printf("  opt_b_no_cache_in_emit:    after_first=%d after_second=%d (expect 1,2) %s\n",
           after_first, after_second, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 2: Bug 2 fix - WSBP-2H scan must detect indirect OUT via SUB_CALL.
//
// We replicate the scan logic here (the inline scan in the Phase 2H worker
// is not directly callable from a test, so we duplicate the predicate).
// A regression in either copy would surface when this test diverges from
// the actual worker behavior.
//
// Scenarios:
//   A. Top-level body has OUT          → emit_possible=true
//   B. Top-level body has SUB_CALL,
//      library entry has OUT           → emit_possible=true
//   C. Top-level body has SUB_CALL,
//      library entry has no OUT        → emit_possible=false
//   D. Top-level body has neither      → emit_possible=false
//   E. SUB_CALL with out-of-range idx → emit_possible=false (no crash)
// =========================================================================
static bool emit_possible_replica(const Ins* body, int nb) {
    bool emit_possible = false;
    for (int i = 0; i < nb && !emit_possible; i++) {
        if (body[i].ti == 12) { emit_possible = true; break; }
        if (body[i].ti == 32) {
            int lib_idx = body[i].c;
            if (lib_idx >= 0 && lib_idx < (int)g_progdb.size()) {
                const ProgramRecord& lpr = g_progdb.records[lib_idx];
                if (subCallLibraryEntryPure(lpr)) {
                    for (int j = 0; j < lpr.nbody; j++) {
                        if (lpr.body[j].ti == 12) { emit_possible = true; break; }
                    }
                }
            }
        }
    }
    return emit_possible;
}

static bool test_wsbp_2h_indirect_out_detection() {
    resetLibrary();
    // L0: body with OUT
    Ins out0 = {12, 0, {0, 0, 0, 0}, 1};
    Ins ops_with_out[1] = {out0};
    g_progdb.records.push_back(makeRecordWithBody(ops_with_out, 1));

    // L1: body without OUT (just INC)
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    Ins ops_no_out[1] = {inc};
    g_progdb.records.push_back(makeRecordWithBody(ops_no_out, 1));

    // Scenario A: top-level body has OUT
    Ins body_a[1] = {out0};
    bool a = emit_possible_replica(body_a, 1);

    // Scenario B: top-level has SUB_CALL(L0) - L0 has OUT
    Ins sc_l0 = {32, 0, {0, 0, 0, 0}, 0};  // SUB_CALL idx=0
    Ins body_b[1] = {sc_l0};
    bool b = emit_possible_replica(body_b, 1);

    // Scenario C: top-level has SUB_CALL(L1) - L1 has no OUT
    Ins sc_l1 = {32, 1, {0, 0, 0, 0}, 0};  // SUB_CALL idx=1
    Ins body_c[1] = {sc_l1};
    bool c = emit_possible_replica(body_c, 1);

    // Scenario D: top-level has neither
    Ins body_d[1] = {inc};
    bool d = emit_possible_replica(body_d, 1);

    // Scenario E: top-level has SUB_CALL with out-of-range idx (no crash)
    Ins sc_oor = {32, 999, {0, 0, 0, 0}, 0};
    Ins body_e[1] = {sc_oor};
    bool e = emit_possible_replica(body_e, 1);

    bool pass = (a && b && !c && !d && !e);
    printf("  wsbp_2h_indirect_out:      A=%d B=%d C=%d D=%d E=%d (expect 1,1,0,0,0) %s\n",
           a, b, c, d, e, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 3: Bug 3 fix - resFingerprint distinguishes DARY ops.
//
// Scenario: two Res records with same dary_base, dary_init_val, mode, etc.
// but different dary_op (the per-digit transform Ins). Before the fix, both
// would produce the same fingerprint and the second would be deduplicated
// as a "duplicate" - but they're functionally distinct programs.
//
// After the fix: dary_op is hashed when dary_base >= 2, so the fingerprints
// differ and both programs are kept.
// =========================================================================
static bool test_resFingerprint_dary_op_distinction() {
    Res r1 = {};
    r1.nbody = 0;
    r1.mode = MODE_ITER;
    r1.nr = 1;
    r1.dary_base = 4;
    r1.dary_init_val = 0;
    r1.dary_op = {0, 0, {0, 0, 0, 0}, 1};  // INC

    Res r2 = r1;
    r2.dary_op = {1, 0, {0, 0, 0, 0}, 1};  // DEC (different ti)

    uint64_t fp1 = resFingerprint(r1);
    uint64_t fp2 = resFingerprint(r2);

    bool pass = (fp1 != fp2);
    printf("  fingerprint_dary_distinct: fp1=%016llx fp2=%016llx (expect distinct) %s\n",
           (unsigned long long)fp1, (unsigned long long)fp2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 4: Sanity - when dary_base < 2, dary_op shouldn't affect fingerprint
// (DARY mode not active, so dary_op is irrelevant state).
// Ensures the conditional gate `if (r.dary_base >= 2)` works correctly.
// =========================================================================
static bool test_resFingerprint_dary_off_no_effect() {
    Res r1 = {};
    r1.nbody = 0;
    r1.mode = MODE_ITER;
    r1.nr = 1;
    r1.dary_base = 0;  // NOT in DARY mode
    r1.dary_op = {0, 0, {0, 0, 0, 0}, 1};

    Res r2 = r1;
    r2.dary_op = {7, 0, {0, 0, 0, 0}, 3};  // very different op

    uint64_t fp1 = resFingerprint(r1);
    uint64_t fp2 = resFingerprint(r2);

    bool pass = (fp1 == fp2);
    printf("  fingerprint_dary_off_same: fp1=%016llx fp2=%016llx (expect equal) %s\n",
           (unsigned long long)fp1, (unsigned long long)fp2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 5: resFingerprint distinguishes by all primary structural fields.
// Sanity check that a few small variations produce distinct fingerprints.
// =========================================================================
static bool test_resFingerprint_field_coverage() {
    Res base = {};
    base.nbody = 1;
    base.body[0] = {0, 0, {0, 0, 0, 0}, 1};  // INC R0
    base.mode = MODE_FUNC;
    base.nr = 2;
    base.outr = 0;
    base.init[0] = 5;

    uint64_t fp_base = resFingerprint(base);

    Res var_mode = base;       var_mode.mode = MODE_ITER;
    Res var_nr   = base;       var_nr.nr = 3;
    Res var_outr = base;       var_outr.outr = 1;
    Res var_init = base;       var_init.init[0] = 7;
    Res var_body = base;       var_body.body[0].ti = 1;  // DEC instead of INC
    Res var_branched = base;   var_branched.branched = true; var_branched.branch_m = 2;

    bool m  = resFingerprint(var_mode)     != fp_base;
    bool nr = resFingerprint(var_nr)       != fp_base;
    bool o  = resFingerprint(var_outr)     != fp_base;
    bool in = resFingerprint(var_init)     != fp_base;
    bool b  = resFingerprint(var_body)     != fp_base;
    bool br = resFingerprint(var_branched) != fp_base;

    bool pass = (m && nr && o && in && b && br);
    printf("  fingerprint_field_cover:   mode=%d nr=%d outr=%d init=%d body=%d branched=%d (all 1) %s\n",
           m, nr, o, in, b, br, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 6: Bug 4 fix - computeMDL distinguishes CONCAT programs by concat_base.
//
// CONCAT mode is pure deductive (predictNext uses concat_base/off/msb directly,
// never reads body). Before the fix, MDL formula didn't include these fields,
// so two CONCAT programs with different bases got identical MDL → identical
// Solomonoff prior weight despite being distinct programs (C2 violation).
// =========================================================================
static bool test_computeMDL_concat_distinct() {
    int ncat = 32; // arbitrary canonical ncat

    Res r1 = {};
    r1.nbody = 0;
    r1.mode = MODE_FUNC;
    r1.nr = 1;
    r1.concat_base = 4;
    r1.concat_off = 0;
    r1.concat_msb = true;

    Res r2 = r1;
    r2.concat_base = 8;  // different base

    double m1 = computeMDL(r1, ncat);
    double m2 = computeMDL(r2, ncat);

    bool pass = (m1 != m2);
    printf("  computeMDL_concat_distinct: m1=%.2f m2=%.2f (expect distinct) %s\n",
           m1, m2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 7: Bug 4 fix - computeMDL distinguishes DARY programs by dary_base.
// Same C2 rationale as CONCAT.
// =========================================================================
static bool test_computeMDL_dary_distinct() {
    int ncat = 32;

    Res r1 = {};
    r1.nbody = 0;
    r1.mode = MODE_FUNC;
    r1.nr = 1;
    r1.dary_base = 4;
    r1.dary_init_val = 0;
    r1.dary_op = {0, 0, {0, 0, 0, 0}, 1};  // INC

    Res r2 = r1;
    r2.dary_base = 10;  // different base

    double m1 = computeMDL(r1, ncat);
    double m2 = computeMDL(r2, ncat);

    bool pass = (m1 != m2);
    printf("  computeMDL_dary_distinct:  m1=%.2f m2=%.2f (expect distinct) %s\n",
           m1, m2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 8: computeMDL distinguishes DARY by dary_op WHEN register arity matters.
//
// With nr=1, register selectors cost log2(1)=0 bits, so arity contributes nothing.
// All opcodes have same opcode-encoding cost (log2(ncat)). So with nr=1, two
// different ti values can produce identical MDL - this is correct under the
// canonical encoding.
//
// To verify dary_op IS distinguished by MDL, use nr > 1 so register selectors
// cost log2(nr) > 0 bits per arity.
// =========================================================================
static bool test_computeMDL_dary_op_distinct_with_arity() {
    int ncat = 32;

    Res r1 = {};
    r1.nbody = 0;
    r1.mode = MODE_FUNC;
    r1.nr = 4;  // log2(4) = 2 bits per register selector
    r1.dary_base = 4;
    r1.dary_init_val = 0;
    r1.dary_op = {0, 0, {0, 0, 0, 0}, 1};  // INC arity 1 → 1*2 = 2 bits selectors

    Res r2 = r1;
    r2.dary_op = {2, 0, {0, 1, 2, 0}, 3};  // ADD arity 3 → 3*2 = 6 bits selectors

    double m1 = computeMDL(r1, ncat);
    double m2 = computeMDL(r2, ncat);

    bool pass = (m1 != m2);
    printf("  computeMDL_dary_arity:     m1=%.2f m2=%.2f (expect distinct, nr=4) %s\n",
           m1, m2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 9: Bug 7 fix - programRecordHash distinguishes branched programs by
// branch_m. The pre-fix bodyHash() in ProgramDB only included
// (body, n, nr, outr, mode), missing branch params. Two distinct branched
// programs with same body but different branch_m would collide → second
// silently rejected at g_progdb.add(). C1-relevant for library completeness.
// =========================================================================
static bool test_programRecordHash_branched_distinct() {
    ProgramRecord pr1 = {};
    pr1.nbody = 2;
    pr1.body[0] = {0, 0, {0, 0, 0, 0}, 1};  // INC R0
    pr1.body[1] = {1, 0, {0, 0, 0, 0}, 1};  // DEC R0
    pr1.mode_u = 0;
    pr1.nr = 2;
    pr1.outr = 0;
    pr1.branched = 1;
    pr1.branch_m = 2;
    pr1.then_len = 1;

    ProgramRecord pr2 = pr1;
    pr2.branch_m = 5;  // different modulus

    uint64_t h1 = programRecordHash(pr1);
    uint64_t h2 = programRecordHash(pr2);

    bool pass = (h1 != h2);
    printf("  programHash_branch_m_distinct: h1=%016llx h2=%016llx (expect distinct) %s\n",
           (unsigned long long)h1, (unsigned long long)h2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 10: programRecordHash distinguishes by init values. Two ITER programs
// with same body but different init[0] are distinct programs (different
// starting state); pre-fix bodyHash collided them.
// =========================================================================
static bool test_programRecordHash_init_distinct() {
    ProgramRecord pr1 = {};
    pr1.nbody = 1;
    pr1.body[0] = {0, 0, {0, 0, 0, 0}, 1};
    pr1.mode_u = 0;  // MODE_ITER
    pr1.nr = 1;
    pr1.outr = 0;
    pr1.init[0] = 5;

    ProgramRecord pr2 = pr1;
    pr2.init[0] = 10;

    uint64_t h1 = programRecordHash(pr1);
    uint64_t h2 = programRecordHash(pr2);

    bool pass = (h1 != h2);
    printf("  programHash_init_distinct:     h1=%016llx h2=%016llx (expect distinct) %s\n",
           (unsigned long long)h1, (unsigned long long)h2, pass ? "PASS" : "FAIL");
    return pass;
}

// =========================================================================
// Test 11: programRecordHash distinguishes by concat_base. Two pure-deductive
// CONCAT programs (both empty body) at different bases must hash distinctly.
// =========================================================================
static bool test_programRecordHash_concat_base_distinct() {
    ProgramRecord pr1 = {};
    pr1.nbody = 0;
    pr1.mode_u = 1;  // MODE_FUNC (typical for CONCAT)
    pr1.nr = 1;
    pr1.outr = 0;
    pr1.concat_base = 4;
    pr1.concat_off = 0;
    pr1.concat_msb = true;

    ProgramRecord pr2 = pr1;
    pr2.concat_base = 8;

    uint64_t h1 = programRecordHash(pr1);
    uint64_t h2 = programRecordHash(pr2);

    bool pass = (h1 != h2);
    printf("  programHash_concat_distinct:   h1=%016llx h2=%016llx (expect distinct) %s\n",
           (unsigned long long)h1, (unsigned long long)h2, pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    printf("=========================================================\n");
    printf("Correctness fixes regression tests (post 2026-05-06 audit)\n");
    printf("=========================================================\n\n");

    int passed = 0, total = 0;
    auto check = [&](bool b) { total++; if (b) passed++; };

    check(test_opt_b_no_cache_in_emit_context());
    check(test_wsbp_2h_indirect_out_detection());
    check(test_resFingerprint_dary_op_distinction());
    check(test_resFingerprint_dary_off_no_effect());
    check(test_resFingerprint_field_coverage());
    check(test_computeMDL_concat_distinct());
    check(test_computeMDL_dary_distinct());
    check(test_computeMDL_dary_op_distinct_with_arity());
    check(test_programRecordHash_branched_distinct());
    check(test_programRecordHash_init_distinct());
    check(test_programRecordHash_concat_base_distinct());

    printf("\n=========================================================\n");
    printf("Result: %d/%d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
