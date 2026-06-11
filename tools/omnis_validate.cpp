// omnis_validate.cpp - Train/test wrapper that validates compression-implies-prediction.
//
// Reads one candidate, splits into train + held-out, runs solve() on the
// training prefix, then asks the discovered program to extend it. Records
// both the training match (sc) and the held-out prediction match (pred_sc),
// classifies the candidate into the Solomonoff contingency cell, and emits
// a single data row.
//
// Definitions (per docs/SOLOMONOFF_VALIDATION.md):
//   K            = max(20, total_terms / 4)
//   train_N      = total_terms - K
//   compresses   = (sc == train_N) AND (mdl < train_N * log2(A))
//   predicts     = (pred_sc == K)              ; strict
//   solomonoff_class:
//      discovered                    if compresses AND predicts
//      compressed_only               if compresses AND NOT predicts
//      not_compressed_predicted      if NOT compresses AND predicts
//      neither                       otherwise
//
// Prediction strategy:
//   - Non-CTX: open-loop via runProgram(r, train_N + K, A); compare positions
//     [train_N, train_N + K) against held-out. This is the strict Solomonoff
//     interpretation: the program's own K continuations must match.
//   - CTX (history-dependent): runProgram returns empty, fall back to
//     teacher-forced predictNext loop. CTX programs encode their dependence
//     on history in ctx_dstar; their MDL already accounts for the seed.
//
// Build:
//   cmake target `omnis_validate` (see CMakeLists.txt).

#include "omnis.cpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// CSV header (kept here as the single source of truth for the schema).
// =============================================================================
//
// Mine-runner / paper-pipeline scripts write this header once at the top of
// their output CSV, then concatenate per-candidate data rows from each
// omnis_validate invocation.

// Provenance columns:
// - `category` and `oeis_xref` are per-row varying provenance (origin of the
//   candidate). Caller passes them via --category / --oeis-xref; defaults are
//   "self" / empty so direct CLI use remains valid.
// - Other §5 fields (omnis_sha, generator_sha, oeis_snapshot_sha, run_id,
//   run_date_utc, host, budget_s, freeze_db) are sweep-level CONSTANTS across
//   all rows of a run - kept in the sibling `.manifest.txt` (DB normalization;
//   no replication × N rows).
static constexpr const char* CSV_HEADER =
    "id,category,oeis_xref,A,total_n,train_n,k,sc,pred_sc,solomonoff_class,"
    "mdl,raw_bits,ratio,time_s,solver_desc";

// =============================================================================

static void printUsage() {
    std::fprintf(stderr,
        "omnis_validate - train/test wrapper for compression-implies-prediction.\n"
        "\n"
        "Usage: omnis_validate [options] [input_file]\n"
        "\n"
        "Reads one sequence from <input_file> (or stdin if absent or '-') in the format:\n"
        "  <id> <A> <N> <t0> <t1> ... <t(N-1)>\n"
        "(same as omnis CLI). Splits last K = max(20, N/4) terms as held-out test.\n"
        "\n"
        "Output: a single CSV data row matching the canonical schema (see --csv-header).\n"
        "\n"
        "Options:\n"
        "  --terms \"v0,v1,...\"   Sequence as comma-separated integers (use with --A).\n"
        "  --A <n>               Alphabet size (required with --terms).\n"
        "  --id <name>           Optional id label for --terms input (default \"input\").\n"
        "  --budget <seconds>    Wall-clock budget for solve() (default 600).\n"
        "  --k-min <n>           Minimum K (default 20).\n"
        "  --k-frac <num/den>    K = max(k_min, total_n*num/den). Default 1/4.\n"
        "  --freeze-db           Read program library but do not add new entries (default ON).\n"
        "  --no-freeze-db        Allow library writes (NOT recommended for benchmark runs).\n"
        "  --db <path>           Program database path (default ../data/program_db.bin).\n"
        "  --csv-header          Print only the CSV header line and exit.\n"
        "  --mixture             Solomonoff weighted-mixture prediction over g_progs\n"
        "                        (programs collected during solve). This is the DEFAULT;\n"
        "                        flag is provided for explicitness/scripting.\n"
        "  --single-best         Use only the lex-best program (legacy / fast path).\n"
        "                        Identical results to --mixture on benchmarks where the\n"
        "                        best program dominates the prior; faster (no\n"
        "                        per-candidate runProgram across g_progs).\n"
        "  --kraft               Print Kraft inequality empirical smoke check on g_progs\n"
        "                        after solve(). Σ 2^(-MDL) ≤ 1 is a necessary (not\n"
        "                        sufficient) condition for a valid prefix code. Violation\n"
        "                        (Σ>1) indicates encoding bug; pass is a smoke test.\n"
        "  --category NAME       Per-row provenance: workload-family name (e.g. 'oeis_core',\n"
        "                        'eca256', 'benchmark14'). Default: 'self' for direct CLI use.\n"
        "                        Recorded verbatim in the output row.\n"
        "  --oeis-xref ID        Per-row provenance: OEIS A-number for OEIS-sourced rows\n"
        "                        (e.g. 'A000005'). Empty for local generators. §5.\n"
        "  --out PATH            Write the CSV row to PATH (append). Default: stdout.\n"
        "                        Recommended for runner scripts because the engine emits\n"
        "                        diagnostic printf to stdout during solve(); --out\n"
        "                        isolates the CSV row.\n"
        "  -h, --help            Show this message.\n"
        "\n"
        "Exit codes: 0 row written, 2 usage error, 3 I/O / parse error.\n"
    );
}

static int readSequence(std::istream& in, std::string& id, int& A, int& N, std::vector<int>& terms) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        if (!(iss >> id >> A >> N)) {
            std::fprintf(stderr, "omnis_validate: malformed input line\n");
            return 3;
        }
        terms.clear(); terms.reserve(N);
        int v;
        while (iss >> v && (int)terms.size() < N) terms.push_back(v);
        if ((int)terms.size() != N) {
            std::fprintf(stderr, "omnis_validate: id=%s expected %d terms, got %d\n",
                         id.c_str(), N, (int)terms.size());
            return 3;
        }
        return 0;
    }
    std::fprintf(stderr, "omnis_validate: no sequence found in input\n");
    return 3;
}

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);

    double budget = 600.0;
    bool freeze_db = true;          // benchmark default: NEVER mutate library
    std::string db_path = "../data/program_db.bin";
    std::string input_path;
    bool inline_terms = false;
    std::string inline_terms_csv;
    int inline_A = -1;
    std::string inline_id = "input";
    bool input_path_set = false;

    int k_min = 20;
    int k_num = 1, k_den = 4;       // K = max(k_min, total_n * num/den)
    std::string out_path;
    // Per-row provenance columns.
    // category: which workload family this candidate came from (oeis_core,
    //           eca256, benchmark14, neg_controls, ...).
    // oeis_xref: OEIS A-number if applicable (e.g. "A000005"); empty for
    //            local generators.
    // Both default to "self" / empty for direct-invocation use.
    std::string category = "self";
    std::string oeis_xref = "";
    // Prediction mode. Default = mixture (rigorous Solomonoff).
    // --single-best opts into the legacy lex-best-only path (faster, identical
    // results when the lex-best program dominates the mixture's prior).
    bool use_single_best = false;
    // Opt-in Kraft inequality smoke check on g_progs.
    bool do_kraft = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help")            { printUsage(); return 0; }
        else if (a == "--csv-header")                   { std::printf("%s\n", CSV_HEADER); return 0; }
        else if (a == "--budget"  && i + 1 < argc)      budget = atof(argv[++i]);
        else if (a == "--freeze-db")                    freeze_db = true;
        else if (a == "--no-freeze-db")                 freeze_db = false;
        else if (a == "--db"      && i + 1 < argc)      db_path = argv[++i];
        else if (a == "--terms"   && i + 1 < argc)      { inline_terms = true; inline_terms_csv = argv[++i]; }
        else if (a == "--A"       && i + 1 < argc)      inline_A = atoi(argv[++i]);
        else if (a == "--id"      && i + 1 < argc)      inline_id = argv[++i];
        else if (a == "--k-min"   && i + 1 < argc)      k_min = atoi(argv[++i]);
        else if (a == "--k-frac"  && i + 1 < argc) {
            std::string s = argv[++i];
            size_t slash = s.find('/');
            if (slash == std::string::npos || slash == 0 || slash == s.size() - 1) {
                std::fprintf(stderr, "omnis_validate: --k-frac must be num/den (e.g. 1/4)\n");
                return 2;
            }
            k_num = atoi(s.substr(0, slash).c_str());
            k_den = atoi(s.substr(slash + 1).c_str());
            if (k_den <= 0 || k_num < 0) { std::fprintf(stderr, "omnis_validate: bad --k-frac\n"); return 2; }
        }
        else if (a == "--mixture")                      use_single_best = false; // explicit, matches default
        else if (a == "--single-best")                  use_single_best = true;
        else if (a == "--kraft")                        do_kraft = true;
        else if (a == "--category"   && i + 1 < argc)   category = argv[++i];
        else if (a == "--oeis-xref"  && i + 1 < argc)   oeis_xref = argv[++i];
        else if (a == "--out"     && i + 1 < argc)      out_path = argv[++i];
        else if (a == "-")                              { input_path.clear(); input_path_set = true; }
        else if (!a.empty() && a[0] != '-' && !input_path_set) { input_path = a; input_path_set = true; }
        else { std::fprintf(stderr, "omnis_validate: unknown option '%s'\n", a.c_str()); printUsage(); return 2; }
    }

    // Read sequence.
    std::string id;
    int A = 0, N = 0;
    std::vector<int> tgt;

    if (inline_terms) {
        if (input_path_set) {
            std::fprintf(stderr, "omnis_validate: cannot combine --terms with file/stdin input\n");
            return 2;
        }
        if (inline_A < 2) {
            std::fprintf(stderr, "omnis_validate: --terms requires --A <n> with n >= 2\n");
            return 2;
        }
        size_t pos = 0;
        while (pos <= inline_terms_csv.size()) {
            size_t comma = inline_terms_csv.find(',', pos);
            std::string tok = inline_terms_csv.substr(pos,
                comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) tgt.push_back(atoi(tok.c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        id = inline_id;
        A  = inline_A;
        N  = (int)tgt.size();
        if (N == 0) { std::fprintf(stderr, "omnis_validate: --terms parsed no values\n"); return 2; }
    } else {
        int rc;
        if (input_path.empty()) {
            rc = readSequence(std::cin, id, A, N, tgt);
        } else {
            std::ifstream f(input_path);
            if (!f.is_open()) { std::fprintf(stderr, "omnis_validate: cannot open '%s'\n", input_path.c_str()); return 3; }
            rc = readSequence(f, id, A, N, tgt);
        }
        if (rc != 0) return rc;
    }

    for (int i = 0; i < N; i++) {
        if (tgt[i] < 0 || tgt[i] >= A) {
            std::fprintf(stderr, "omnis_validate: term[%d]=%d out of [0, %d)\n", i, tgt[i], A);
            return 3;
        }
    }

    // Train/test split.
    int K = std::max(k_min, (N * k_num) / k_den);
    if (K >= N) {
        std::fprintf(stderr, "omnis_validate: K=%d >= N=%d (need more total terms)\n", K, N);
        return 3;
    }
    int train_N = N - K;
    if (train_N < k_min) {
        std::fprintf(stderr, "omnis_validate: train_N=%d below floor=%d\n", train_N, k_min);
        return 3;
    }
    std::vector<int> train(tgt.begin(), tgt.begin() + train_N);
    std::vector<int> test(tgt.begin() + train_N, tgt.end());

    // Engine setup.
    G_PROGDB_PATH = db_path.c_str();
    g_progdb = ProgramDB::load(db_path.c_str());
    int ncat = 0;
    {
        const auto& ranges = isaConstantRanges();
        for (auto& [t, cs] : ranges) ncat += (int)cs.size();
        int sz_lib = subCallCatalogSize();
        for (int idx = 0; idx < sz_lib; idx++)
            if (subCallLibraryEntryInvocable(idx)) ncat++;
    }

    // Solve on training prefix.
    g_progs.clear();
    g_progs_fingerprints.clear();
    g_bench_prog_start = 0;
    double t0 = now_s();
    Res r = solve(train, A, now_s() + budget);
    double dt = now_s() - t0;
    r.mdl = computeMDL(r, ncat);

    // Kraft inequality smoke check on g_progs (opt-in via --kraft).
    // Must run BEFORE pred_sc compute so the diagnostic appears in stdout
    // alongside other solve-time diagnostics, not interleaved with the CSV row.
    if (do_kraft) {
        kraftSmokeCheck(g_progs, stdout);
    }

    // Compute pred_sc.
    //
    // Two strategies, both AUTOREGRESSIVE (program's own output feeds back -
    // strict Solomonoff: "the program continues correctly"):
    //
    //   1. Non-CTX: runProgram(r, train_N + K, A) generates a fresh
    //      train_N + K outputs from program init. We compare positions
    //      [train_N, train_N + K).
    //
    //   2. MODE_CTX: runProgram returns empty (cannot generate without seed).
    //      We replicate verifyCTX's autoregressive loop here: seed `gen` with
    //      the full training prefix, then for each test step compute R from
    //      gen[t-ds+perm[k]], run body, push the program's output back into
    //      gen. This is open-loop - we feed the PROGRAM's output back, not
    //      the test target. Matches the strict Solomonoff prediction
    //      semantics from the engine's canonical interpreter.
    //
    // EARLIER BUG: the previous version used predictNext as the CTX fallback.
    // predictNext bails out via `if (r.sc < N) return -1` at the top, where
    // N = ext.size(). Since r.sc is fixed at train_N, every step >= 1 (which
    // grows N) returned -1 instantly. Result: pred_sc capped at 1 for all
    // CTX programs regardless of correctness. Found during a review
    // audit when trimod8/collatz showed pred_sc=1.
    int sc = r.sc;
    int pred_sc = 0;
    bool used_ctx_path = false;

    if (!use_single_best) {
        // DEFAULT: Solomonoff weighted-mixture prediction.
        //
        // g_progs holds all sc=N programs found during solve() (Phase 0 / 1A /
        // 1B / 2A / 2B / 2C / 2F / 2H all call recordProg via #96 plumbing),
        // each with computed MDL. Mixture vote-weights each program by 2^(-mdl_p)
        // and aggregates predictions per test position via weighted majority.
        //
        // For canonical b14np: produces identical results to --single-best
        // because the lex-best program dominates the mixture (one program with
        // MDL X dominates a competitor at MDL X+10 by a factor of 2^10 = 1024).
        // For workloads where multiple sc=N programs are within ~1-2 bits of
        // each other, mixture is the rigorous Solomonoff prediction.
        //
        // Empty mixture (g_progs == empty, sc < train_N) returns -1 sentinels
        // → pred_sc stays 0. Matches single-best semantics for that case.
        std::vector<int> mix = predictMixture(g_progs, train, K, A);
        for (int k = 0; k < K; k++) if (mix[k] == test[k]) pred_sc++;
    } else if (sc == train_N) {
        // --single-best: lex-best-only prediction (legacy / fast path).
        std::vector<int> out = runProgram(r, train_N + K, A);
        if ((int)out.size() == train_N + K) {
            for (int k = 0; k < K; k++) if (out[train_N + k] == test[k]) pred_sc++;
        } else if (r.mode == MODE_CTX && r.ctx_dstar > 0 && r.nr > 0 && r.nbody > 0) {
            used_ctx_path = true;
            std::vector<int> gen = train;  // ctx history is the full training prefix
            int ds = r.ctx_dstar;
            for (int k = 0; k < K; k++) {
                int t = train_N + k;
                int64_t R[kRegisterCount] = {};
                for (int j = 0; j < r.nr; j++) {
                    int idx = t - ds + r.ctx_perm[j];
                    if (idx < 0 || idx >= (int)gen.size()) { idx = 0; }  // defensive; should never trip
                    R[j] = gen[idx];
                }
                g_sat = false;
                exBody(R, r.body, r.nbody);
                if (g_sat) break;
                int v = pm(R[r.outr], A);
                if (v == test[k]) pred_sc++;
                gen.push_back(v);  // autoregressive: feed program's own output
            }
        }
        // If we got here without entering either branch, prediction is 0
        // (e.g., a deductive mode whose runProgram returned the wrong size
        // - should not happen in practice).
    }
    // If sc < train_N (didn't compress training), pred_sc stays 0.

    // Classify.
    double train_raw_bits = train_N * std::log2(std::max(2, A));
    double total_raw_bits = N * std::log2(std::max(2, A));
    bool compresses = (sc == train_N) && (r.mdl < train_raw_bits);
    bool predicts   = (pred_sc == K);
    const char* cls = compresses
        ? (predicts ? "discovered" : "compressed_only")
        : (predicts ? "not_compressed_predicted" : "neither");

    double ratio = (total_raw_bits > 0) ? r.mdl / total_raw_bits : 999.0;

    // Escape solver_desc for CSV (quote if contains comma/quote/newline).
    auto csv_escape = [](const std::string& s) -> std::string {
        bool needs = s.find_first_of(",\"\n") != std::string::npos;
        if (!needs) return s;
        std::string out = "\"";
        for (char c : s) { if (c == '"') out += '"'; out += c; }
        out += "\"";
        return out;
    };

    // Compose CSV row. Column order matches CSV_HEADER exactly:
    //   id, category, oeis_xref, A, total_n, train_n, k, sc, pred_sc,
    //   solomonoff_class, mdl, raw_bits, ratio, time_s, solver_desc
    // category and oeis_xref are per-row provenance.
    char row_buf[1024];
    int row_len = std::snprintf(row_buf, sizeof(row_buf),
        "%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,%.2f,%.2f,%.4f,%.3f,%s\n",
        csv_escape(id).c_str(),
        csv_escape(category).c_str(),
        csv_escape(oeis_xref).c_str(),
        A, N, train_N, K, sc, pred_sc, cls,
        r.mdl, total_raw_bits, ratio, dt, csv_escape(r.desc).c_str());
    if (row_len < 0 || (size_t)row_len >= sizeof(row_buf)) {
        std::fprintf(stderr, "omnis_validate: CSV row too long (id may have unusually long fields)\n");
        return 3;
    }

    // Write to stdout (default) or to --out path. Engine printf during solve()
    // already went to stdout; --out PATH isolates the structured row.
    if (out_path.empty()) {
        std::fwrite(row_buf, 1, (size_t)row_len, stdout);
        std::fflush(stdout);
    } else {
        // Append, with O_APPEND-equivalent semantics for concurrent writers.
        std::FILE* f = std::fopen(out_path.c_str(), "a");
        if (!f) {
            std::fprintf(stderr, "omnis_validate: cannot open --out '%s' for append\n", out_path.c_str());
            return 3;
        }
        std::fwrite(row_buf, 1, (size_t)row_len, f);
        std::fclose(f);
    }

    if (used_ctx_path) {
        std::fprintf(stderr, "omnis_validate: %s used CTX autoregressive path\n", id.c_str());
    }

    // Engine boundary: respect freeze_db. Default is ON; never write under
    // benchmark conditions even if a new program was discovered.
    if (!freeze_db && sc == train_N) {
        g_progdb.add(r, ncat, train, A);
        g_progdb.save(db_path.c_str());
    }

    return 0;
}
