// =========================================================================
// SUB_CALL extension verification (canonical engine)
//
// SUB_CALL (ti=32) inline-expands a g_progdb library entry into the calling
// register state. Library index lives in `c`; ar=0 (no register operands).
//
// Filtering policy (subCallLibraryEntryPure):
//   - branched, concat, dary, step entries → filter (special-mode bodies)
//   - MODE_CTX entries → filter (require context-permuted reload per iteration)
//   - empty bodies (nbody==0) → filter
//   - bodies containing ti=32 → filter (recursion guard)
//
// Build:
//   g++ -std=c++17 -O2 -I../src test_omega_sub_call.cpp -o /tmp/test_omega_sub_call
// =========================================================================

#define main omnis_main_unused
#include "omnis.cpp"
#undef main

#include <cstdio>

// Reset g_progdb to a known state for each test.
static void resetLibrary() {
    g_progdb.records.clear();
    g_progdb.body_hashes.clear();
}

// Helper: construct a ProgramRecord with body=ops, mode=MODE_ITER, no special modes.
static ProgramRecord makeRecord(const Ins* ops, int n,
                                int mode = (int)MODE_ITER,
                                bool branched = false,
                                int concat_base = 0,
                                int dary_base = 0,
                                int step_off = 0) {
    ProgramRecord pr = {};
    pr.nbody = (uint8_t)n;
    for (int i = 0; i < n; i++) pr.body[i] = ops[i];
    pr.nr = 1;
    pr.outr = 0;
    pr.mode_u = (uint8_t)mode;
    pr.ointerp_u = 0;
    pr.branched = branched ? 1 : 0;
    pr.concat_base = concat_base;
    pr.dary_base = dary_base;
    pr.step_off = step_off;
    return pr;
}

// Test 1: empty library → SUB_CALL is no-op (does not touch registers, does not crash).
static bool test_empty_library_no_op() {
    resetLibrary();
    int64_t R[kRegisterCount] = {};
    R[0] = 42;
    Ins sub_call = {32, 0, {0, 0, 0, 0}, 0};
    ex(R, sub_call);
    bool pass = (R[0] == 42);  // unchanged: empty library → exSubCall returns early
    printf("  empty library no-op:           R0=%lld (expect 42) %s\n",
           (long long)R[0], pass ? "PASS" : "FAIL");
    return pass;
}

// Test 2: basic invocable entry - INC R0 body. SUB_CALL(L0) increments R0.
static bool test_basic_inline() {
    resetLibrary();
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};   // INC R0
    auto pr = makeRecord(&inc, 1);
    g_progdb.records.push_back(pr);

    int64_t R[kRegisterCount] = {};
    R[0] = 5;
    Ins sub_call = {32, 0, {0, 0, 0, 0}, 0};
    ex(R, sub_call);
    bool pass = (R[0] == 6);   // INC applied via inline expansion
    printf("  basic inline (INC):            R0=%lld (expect 6) %s\n",
           (long long)R[0], pass ? "PASS" : "FAIL");
    return pass;
}

// Test 3: recursion guard - entry containing ti=32 is filtered, so SUB_CALL is no-op.
static bool test_recursion_guard() {
    resetLibrary();
    Ins inner_subcall = {32, 0, {0, 0, 0, 0}, 0}; // body uses SUB_CALL itself
    auto pr_recursive = makeRecord(&inner_subcall, 1);
    g_progdb.records.push_back(pr_recursive);

    int64_t R[kRegisterCount] = {};
    R[0] = 100;
    Ins outer_call = {32, 0, {0, 0, 0, 0}, 0};
    ex(R, outer_call);
    // Filter rejects the recursive entry → exSubCall returns without executing.
    bool pass = (R[0] == 100);
    printf("  recursion guard (no-op):       R0=%lld (expect 100) %s\n",
           (long long)R[0], pass ? "PASS" : "FAIL");
    return pass;
}

// Test 4: branched-mode library entry is filtered.
static bool test_branched_filter() {
    resetLibrary();
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    auto pr = makeRecord(&inc, 1, /*mode*/ 0, /*branched*/ true);
    g_progdb.records.push_back(pr);

    int64_t R[kRegisterCount] = {};
    R[0] = 7;
    Ins sub_call = {32, 0, {0, 0, 0, 0}, 0};
    ex(R, sub_call);
    // Branched entries are filtered → no inline expansion → R[0] unchanged.
    bool pass = (R[0] == 7);
    printf("  branched filter:               R0=%lld (expect 7 filtered) %s\n",
           (long long)R[0], pass ? "PASS" : "FAIL");
    return pass;
}

// Test 5: MODE_CTX library entry is filtered.
static bool test_mode_ctx_filter() {
    resetLibrary();
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    auto pr = makeRecord(&inc, 1, (int)MODE_CTX);
    g_progdb.records.push_back(pr);

    int64_t R[kRegisterCount] = {};
    R[0] = 11;
    Ins sub_call = {32, 0, {0, 0, 0, 0}, 0};
    ex(R, sub_call);
    // MODE_CTX entries are filtered → R[0] unchanged.
    bool pass = (R[0] == 11);
    printf("  MODE_CTX filter:               R0=%lld (expect 11 filtered) %s\n",
           (long long)R[0], pass ? "PASS" : "FAIL");
    return pass;
}

// Test 6: typeAr(32) == 0 and writerArgIdx == -1.
static bool test_typeAr_writerArgIdx() {
    int ar = typeAr(32);
    Ins fake = {32, 0, {0, 0, 0, 0}, 0};
    int wi = writerArgIdx(fake);
    bool pass = (ar == 0) && (wi == -1);
    printf("  typeAr(32)=%d wIdx=%d (expect 0,-1) %s\n",
           ar, wi, pass ? "PASS" : "FAIL");
    return pass;
}

// Test 7: catalog accounting - buildL1 emits one Ins per invocable library entry.
static bool test_catalog_invocable_count() {
    resetLibrary();

    // Add 3 invocable entries (INC bodies) and 2 non-invocable (one branched, one MODE_CTX).
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    g_progdb.records.push_back(makeRecord(&inc, 1));                          // L0 invocable
    g_progdb.records.push_back(makeRecord(&inc, 1));                          // L1 invocable
    g_progdb.records.push_back(makeRecord(&inc, 1));                          // L2 invocable
    g_progdb.records.push_back(makeRecord(&inc, 1, 0, /*branched*/ true));    // L3 filtered
    g_progdb.records.push_back(makeRecord(&inc, 1, (int)MODE_CTX));            // L4 filtered

    int subcall_count_in_l1 = 0;
    auto L1 = buildL1(2);
    for (auto& ins : L1) if (ins.ti == 32) subcall_count_in_l1++;

    bool pass = (subcall_count_in_l1 == 3);
    printf("  catalog invocable count:       %d SUB_CALL slots (expect 3) %s\n",
           subcall_count_in_l1, pass ? "PASS" : "FAIL");
    return pass;
}

// Test 8: wide-int (W) variant - SUB_CALL via exW propagates writes.
static bool test_subcallW() {
    resetLibrary();
    Ins inc = {0, 0, {0, 0, 0, 0}, 1};
    auto pr = makeRecord(&inc, 1);
    g_progdb.records.push_back(pr);

    W R[kRegisterCount];
    R[0] = W::from(50);
    Ins sub_call = {32, 0, {0, 0, 0, 0}, 0};
    exW(R, sub_call);
    // exBodyW INC: R[0] = R[0] + W::from(1). 50 → 51.
    bool pass = (R[0].w[0] == 51);
    printf("  exW basic inline:              R0.lo=%llu (expect 51) %s\n",
           (unsigned long long)R[0].w[0], pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    printf("=========================================================\n");
    printf("SUB_CALL - Canonical engine verification\n");
    printf("=========================================================\n\n");

    int passed = 0, total = 0;
    auto check = [&](bool b) { total++; if (b) passed++; };

    check(test_empty_library_no_op());
    check(test_basic_inline());
    check(test_recursion_guard());
    check(test_branched_filter());
    check(test_mode_ctx_filter());
    check(test_typeAr_writerArgIdx());
    check(test_catalog_invocable_count());
    check(test_subcallW());

    printf("\n=========================================================\n");
    printf("Result: %d/%d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
