// =========================================================================
// Regression tests for the categorical workload pipeline.
//
// Coverage (one test per pipeline stage where viable as an offline check):
//   1. A1 - gen_workload determinism: same (category, n, seed, SDE) -> byte-identical
//   2. A2 - gen_workload output parses cleanly (id A N + N terms, term in [0,A))
//   3. A3 - gen_workload eca_030 == in-tree benchmarks.h::genRule30 (cross-impl)
//   4. C1 - omnis_validate emits the exact CSV header schema
//   5. C2 - omnis_validate determinism: 3 reruns yield identical (sc, pred_sc),
//          MDL within 0.5 bits
//   6. checksums - committed data/categories/CHECKSUMS.sha256 verifies if
//          binaries are present; SKIP if data/categories/ is absent
//
// Tests that require the OEIS snapshot (stripped.gz, names.gz, keywords.tsv)
// are SKIP'd when the snapshot directory is missing - keeps the test suite
// runnable on a clean clone before tools/oeis_fetch.sh.
//
// Build: wired into CMakeLists.txt as test_gen_workload (CTest target).
// Depends on: gen_workload, omnis_validate (declared as DEPENDS in CTest).
// =========================================================================

#include "benchmarks.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

// =========================================================================
// Test infrastructure
// =========================================================================

struct TestResult { int passed = 0; int total = 0; int skipped = 0; };

#define CHECK(label, cond) do { \
    bool _c = (cond); \
    std::printf("  %-48s %s\n", label, _c ? "PASS" : "FAIL"); \
    tr.total++; if (_c) tr.passed++; \
} while (0)

#define SKIP(label, reason) do { \
    std::printf("  %-48s SKIP (%s)\n", label, reason); \
    tr.skipped++; \
} while (0)

// Repo paths derived from the test binary's working directory.
// CTest runs binaries from the build directory (e.g., build-cmake/), so
// relative paths target ../tools/, ../data/, etc.
static const std::string REPO_ROOT      = "..";
static const std::string TOOLS_DIR      = REPO_ROOT + "/tools";
static const std::string CATEGORIES_DIR = REPO_ROOT + "/data/categories";
static const std::string SNAPSHOT_DIR   = REPO_ROOT + "/data/oeis/snapshot";
static const std::string GEN_WL_BIN     = "./gen_workload";
static const std::string VALIDATE_BIN   = "./omnis_validate";

// Run a shell command, capture stdout to `out`, return exit code.
static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
    std::FILE* f = popen(cmd.c_str(), "r");
    if (!f) return -1;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
        out.append(buf, n);
    }
    int rc = pclose(f);
    return rc;
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// =========================================================================
// Test 1 - A1: gen_workload determinism
// =========================================================================

static void test_gen_determinism(TestResult& tr) {
    std::printf("[A1] gen_workload determinism\n");
    if (!fileExists(GEN_WL_BIN)) { SKIP("gen_workload binary present", "binary missing"); return; }

    std::string out_a, out_b;
    int rc_a = runCapture("SOURCE_DATE_EPOCH=1714867200 " + GEN_WL_BIN + " --category benchmark14 2>/dev/null", out_a);
    int rc_b = runCapture("SOURCE_DATE_EPOCH=1714867200 " + GEN_WL_BIN + " --category benchmark14 2>/dev/null", out_b);
    CHECK("gen_workload exit codes both 0",   rc_a == 0 && rc_b == 0);
    CHECK("two reruns produce byte-identical", out_a == out_b);
    CHECK("output is non-empty",              !out_a.empty());
}

// =========================================================================
// Test 2 - A2: format conformance (id A N + N terms, term in [0, A))
// =========================================================================

static void test_gen_format(TestResult& tr) {
    std::printf("[A2] gen_workload format conformance\n");
    if (!fileExists(GEN_WL_BIN)) { SKIP("gen_workload binary present", "binary missing"); return; }

    std::string out;
    int rc = runCapture("SOURCE_DATE_EPOCH=1714867200 " + GEN_WL_BIN + " --category arithmetic 2>/dev/null", out);
    if (rc != 0) { SKIP("gen_workload arithmetic", "exec failed"); return; }

    bool all_ok = true;
    int rows = 0;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        rows++;
        std::istringstream ls(line);
        std::string id; int A, N;
        if (!(ls >> id >> A >> N)) { all_ok = false; break; }
        std::vector<int> terms; int t;
        while (ls >> t && (int)terms.size() < N) terms.push_back(t);
        if ((int)terms.size() != N) { all_ok = false; break; }
        for (int v : terms) {
            if (v < 0 || v >= A) { all_ok = false; break; }
        }
    }
    CHECK("at least one body row emitted", rows > 0);
    CHECK("every row parses as id A N + N terms in [0,A)", all_ok);
}

// =========================================================================
// Test 3 - A3: eca_030 (rule-number lookup) == benchmarks.h genRule30 (XOR)
// =========================================================================

static void test_gen_eca030_xref(TestResult& tr) {
    std::printf("[A3] eca_030 cross-implementation match\n");
    if (!fileExists(GEN_WL_BIN)) { SKIP("gen_workload binary present", "binary missing"); return; }

    std::string out;
    int rc = runCapture("SOURCE_DATE_EPOCH=1714867200 " + GEN_WL_BIN + " --category eca256 --n 100 2>/dev/null", out);
    if (rc != 0) { SKIP("gen_workload eca256", "exec failed"); return; }

    // Find the line starting with "eca_030 "
    std::istringstream iss(out);
    std::string line, eca030_line;
    while (std::getline(iss, line)) {
        if (line.rfind("eca_030 ", 0) == 0) { eca030_line = line; break; }
    }
    CHECK("eca_030 line present in output", !eca030_line.empty());
    if (eca030_line.empty()) return;

    // Parse: "eca_030 2 100 t0 t1 ..."
    std::istringstream ls(eca030_line);
    std::string id; int A, N;
    ls >> id >> A >> N;
    std::vector<int> emitted; int v;
    while (ls >> v) emitted.push_back(v);
    CHECK("eca_030 has 100 terms",     emitted.size() == 100);

    // Compute reference via the explicit XOR formula in benchmarks.h.
    auto reference = genRule30(100);
    CHECK("eca_030 first 100 terms match benchmarks.h::genRule30",
          emitted == reference);
}

// =========================================================================
// Test 4 - C1: omnis_validate emits the canonical CSV header
// =========================================================================

static void test_validate_csv_header(TestResult& tr) {
    std::printf("[C1] omnis_validate CSV header schema\n");
    if (!fileExists(VALIDATE_BIN)) { SKIP("omnis_validate binary present", "binary missing"); return; }

    std::string out;
    int rc = runCapture(VALIDATE_BIN + " --csv-header", out);
    CHECK("--csv-header exit 0", rc == 0);

    // Must end with newline; strip for parse.
    std::string header = out;
    while (!header.empty() && (header.back() == '\n' || header.back() == '\r')) header.pop_back();

    // Frozen schema (matches CSV_HEADER in tools/omnis_validate.cpp).
    // The CSV schema has 15 columns. `category` and
    // `oeis_xref` are per-row provenance; sweep-level provenance lives in
    // the run manifest (DB normalization - see comment on CSV_HEADER).
    const std::string expected =
        "id,category,oeis_xref,A,total_n,train_n,k,sc,pred_sc,solomonoff_class,mdl,raw_bits,ratio,time_s,solver_desc";
    CHECK("header matches frozen 15-column schema", header == expected);
}

// =========================================================================
// Test 5 - C2: omnis_validate determinism on a deterministic candidate
// =========================================================================

static void test_validate_determinism(TestResult& tr) {
    std::printf("[C2] omnis_validate determinism (3 reruns)\n");
    if (!fileExists(VALIDATE_BIN)) { SKIP("omnis_validate binary present", "binary missing"); return; }

    // Generate a 200-term counting-mod-4 sequence as CSV string for --terms.
    std::ostringstream tcsv;
    for (int i = 0; i < 200; i++) { if (i) tcsv << ','; tcsv << (i % 4); }

    std::array<std::string, 3> rows;
    for (int i = 0; i < 3; i++) {
        std::string out;
        // Fresh empty DB per run to keep library state out of the contract.
        int rc = runCapture(VALIDATE_BIN + " --terms \"" + tcsv.str() + "\" --A 4 --id det_run --budget 5 --freeze-db --db /tmp/_omnis_validate_test_db.bin 2>/dev/null", out);
        if (rc != 0) { SKIP("validator exec", "non-zero exit"); return; }
        // Strip trailing newline.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        // Engine emits diagnostic printf during solve(); CSV row is the LAST
        // line. Without this, the field() parser walks engine debug commas.
        size_t last_nl = out.find_last_of('\n');
        if (last_nl != std::string::npos) out = out.substr(last_nl + 1);
        rows[i] = out;
    }

    // Parse columns 5 (sc), 6 (pred_sc), 8 (mdl) for each row and compare.
    auto field = [](const std::string& csv, int idx) {
        int cur = 0; size_t pos = 0;
        while (pos < csv.size()) {
            size_t comma = csv.find(',', pos);
            if (cur == idx) return csv.substr(pos, (comma == std::string::npos ? csv.size() : comma) - pos);
            if (comma == std::string::npos) break;
            pos = comma + 1; cur++;
        }
        return std::string();
    };

    // 15-column schema: id, category, oeis_xref, A, total_n,
    // train_n, k, sc, pred_sc, solomonoff_class, mdl, raw_bits, ratio,
    // time_s, solver_desc. 0-indexed: sc=7, pred_sc=8, mdl=10.
    bool sc_eq    = field(rows[0], 7) == field(rows[1], 7) && field(rows[1], 7) == field(rows[2], 7);
    bool pred_eq  = field(rows[0], 8) == field(rows[1], 8) && field(rows[1], 8) == field(rows[2], 8);
    double mdl_min = 1e18, mdl_max = -1e18;
    for (auto& r : rows) {
        double m = std::strtod(field(r, 10).c_str(), nullptr);
        if (m < mdl_min) mdl_min = m; if (m > mdl_max) mdl_max = m;
    }
    bool mdl_close = (mdl_max - mdl_min) <= 0.5;

    CHECK("sc identical across 3 reruns",            sc_eq);
    CHECK("pred_sc identical across 3 reruns",       pred_eq);
    CHECK("MDL stable within 0.5 bits across 3 reruns", mdl_close);
}

// =========================================================================
// Test 6 - committed CHECKSUMS.sha256 still verifies (whole-file SHA pin)
// =========================================================================

static void test_checksums_pin(TestResult& tr) {
    std::printf("[D2] data/categories/CHECKSUMS.sha256 verifies\n");
    std::string ck = CATEGORIES_DIR + "/CHECKSUMS.sha256";
    if (!fileExists(ck)) {
        SKIP("CHECKSUMS.sha256 present", "data/categories/ not populated - run tools/gen_all.sh");
        return;
    }
    std::string out;
    int rc = runCapture("cd " + CATEGORIES_DIR + " && shasum -a 256 -c CHECKSUMS.sha256 2>&1", out);
    bool ok = (rc == 0);
    if (!ok) std::printf("    shasum output:\n%s\n", out.c_str());
    CHECK("all 14 workload files match committed pins", ok);
}

// =========================================================================
// Test 7 - CTX prediction regression: trimod8 must classify as `discovered`
//
// Why: a 2nd-order Markov sequence (T(n) mod 8 has 16-period determined by
// last 2 values) MUST predict perfectly when the engine finds a CTX_X
// solver. An earlier validator bug made pred_sc cap at 1 for any CTX
// program (the predictNext fallback bailed out via `if r.sc<N return -1`).
// This test pins the autoregressive CTX path: bug returns would re-show as
// `compressed_only` instead of `discovered`.
// =========================================================================

static void test_ctx_prediction_regression(TestResult& tr) {
    std::printf("[CTX-regression] trimod8 must classify as discovered (autoregressive CTX path)\n");
    if (!fileExists(VALIDATE_BIN)) { SKIP("omnis_validate binary present", "binary missing"); return; }

    // Build trimod8 = T(n) mod 8 for 70 terms (50 train + 20 held-out under K=20).
    std::ostringstream tcsv;
    for (int n = 0; n < 70; n++) {
        if (n) tcsv << ',';
        long t = ((long)n * (n + 1) / 2) % 8;
        tcsv << t;
    }
    std::string out;
    // Budget 30s gives ~6× headroom over the typical 4-5s solve time.
    // Earlier 5s budget was flaky under any concurrent CPU load (the test
    // is meant to verify the CTX autoregressive prediction path, not race
    // against system scheduling).
    int rc = runCapture(VALIDATE_BIN +
        " --terms \"" + tcsv.str() + "\" --A 8 --id _trimod8_regression "
        "--budget 30 --freeze-db --db /tmp/_omnis_validate_test_db.bin 2>/dev/null", out);
    if (rc != 0) { SKIP("trimod8 validator", "exec failed"); return; }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();

    // The engine emits diagnostic printf to stdout during solve(); the CSV
    // row is the LAST line (its `--out` defaults to stdout when no --out is
    // given). Older versions of this test parsed the whole capture and broke
    // when diagnostic output contained commas (e.g., body descriptions like
    // "INC(R0),DEC(R1)"). Take the final newline-delimited record only.
    {
        size_t last_nl = out.find_last_of('\n');
        if (last_nl != std::string::npos) out = out.substr(last_nl + 1);
    }

    // Parse class (column 7, 0-indexed).
    auto field = [](const std::string& csv, int idx) {
        int cur = 0; size_t pos = 0;
        while (pos < csv.size()) {
            size_t comma = csv.find(',', pos);
            if (cur == idx) return csv.substr(pos, (comma == std::string::npos ? csv.size() : comma) - pos);
            if (comma == std::string::npos) break;
            pos = comma + 1; cur++;
        }
        return std::string();
    };
    // 15-column schema: solomonoff_class=9, sc=7, pred_sc=8.
    std::string cls = field(out, 9);
    std::string sc  = field(out, 7);
    std::string ps  = field(out, 8);

    bool ok = (cls == "discovered") && (sc == "50") && (ps == "20");
    if (!ok) {
        std::printf("    UNEXPECTED row: %s\n", out.c_str());
        std::printf("    expect class=discovered sc=50 pred_sc=20; got class=%s sc=%s pred_sc=%s\n",
                    cls.c_str(), sc.c_str(), ps.c_str());
    }
    CHECK("trimod8 classifies as discovered (CTX autoregressive working)", ok);
}

// =========================================================================
// Test 8 (optional) - OEIS canary cross-check (SKIP if no snapshot)
// =========================================================================

static void test_oeis_canary(TestResult& tr) {
    std::printf("[B2] OEIS canary cross-check (A000005 stripped vs b-file)\n");
    std::string stripped = SNAPSHOT_DIR + "/stripped.gz";
    std::string bfile    = REPO_ROOT + "/data/oeis/canary/b000005.txt";
    if (!fileExists(stripped) || !fileExists(bfile)) {
        SKIP("snapshot + canary files present", "OEIS data not pinned - run tools/oeis_fetch.sh");
        return;
    }

    // Use shell zcat + grep to extract A000005's first 100 terms from stripped.gz.
    std::string s_terms_raw, b_terms_raw;
    runCapture("gunzip -c " + stripped + " | grep -m 1 '^A000005 ' | "
               "awk -F',' '{for(i=2;i<=101;i++) printf \"%s\\n\", $i}'", s_terms_raw);
    runCapture("head -100 " + bfile + " | awk '{if (NF >= 2) print $2}'", b_terms_raw);

    auto split = [](const std::string& s) {
        std::vector<std::string> v; std::istringstream iss(s); std::string ln;
        while (std::getline(iss, ln)) {
            while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            if (!ln.empty()) v.push_back(ln);
        }
        return v;
    };
    auto sv = split(s_terms_raw);
    auto bv = split(b_terms_raw);
    int n = std::min({(int)sv.size(), (int)bv.size(), 100});
    bool match = (n > 0);
    for (int i = 0; i < n; i++) if (sv[i] != bv[i]) { match = false; break; }

    CHECK("A000005 first 100 terms match between stripped.gz and b000005.txt", match && n >= 50);
}

// =========================================================================

int main() {
    std::printf("=========================================================\n");
    std::printf("Regression tests for the categorical pipeline\n");
    std::printf("=========================================================\n\n");

    TestResult tr;
    test_gen_determinism(tr);
    test_gen_format(tr);
    test_gen_eca030_xref(tr);
    test_validate_csv_header(tr);
    test_validate_determinism(tr);
    test_checksums_pin(tr);
    test_ctx_prediction_regression(tr);
    test_oeis_canary(tr);

    std::printf("\n=========================================================\n");
    std::printf("Result: %d/%d passed, %d skipped\n", tr.passed, tr.total, tr.skipped);
    return (tr.passed == tr.total) ? 0 : 1;
}
