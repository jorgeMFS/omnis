// cli.cpp — minimal command-line wrapper around the OMNIS engine.
//
// Reads one sequence (from a file or stdin), runs solve(), prints the result.
// Multi-target processing is left to shell composition (xargs, while loops).
//
// Format for input:    <id> <A> <N> <t0> <t1> ... <t(N-1)>
//                      (single line; '#' at start of line = comment)
//
// Build:
//   g++ -std=c++17 -O3 -march=native -I../src ../src/cli.cpp -o omnis

#include "omnis.cpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void printUsage() {
    fprintf(stderr,
        "OMNIS — Observational Minimal-length Nonparametric Inductive Synthesis\n"
        "\n"
        "Usage: omnis [options] [input_file]\n"
        "\n"
        "Reads one sequence from <input_file> (or stdin if absent or '-') in the format:\n"
        "  <id> <A> <N> <t0> <t1> ... <t(N-1)>\n"
        "Lines beginning with '#' are ignored.\n"
        "\n"
        "Outputs the discovered program, or PARTIAL if no exact-match program was found\n"
        "within the budget. Exit code: 0 = solved, 1 = partial, 2 = usage, 3 = I/O.\n"
        "\n"
        "Options:\n"
        "  --terms \"v0,v1,...\"   Sequence as comma-separated integers (use with --A).\n"
        "  --A <n>               Alphabet size (required with --terms).\n"
        "  --id <name>           Optional id label for --terms input (default \"input\").\n"
        "  --budget <seconds>    Wall-clock budget (default 600).\n"
        "  --freeze-db           Read program library but do not add new entries.\n"
        "  --db <path>           Program database path (default ../data/program_db.bin).\n"
        "  --json                Emit machine-readable output.\n"
        "  -h, --help            Show this message.\n"
    );
}

static int readSequence(std::istream& in, std::string& id, int& A, int& N, std::vector<int>& terms) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        if (!(iss >> id >> A >> N)) {
            fprintf(stderr, "omnis: malformed input line (expected: <id> <A> <N> <terms...>)\n");
            return 3;
        }
        terms.clear(); terms.reserve(N);
        int v;
        while (iss >> v && (int)terms.size() < N) terms.push_back(v);
        if ((int)terms.size() != N) {
            fprintf(stderr, "omnis: id=%s expected %d terms, got %d\n",
                    id.c_str(), N, (int)terms.size());
            return 3;
        }
        return 0;
    }
    fprintf(stderr, "omnis: no sequence found in input\n");
    return 3;
}

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);

    double budget = 600.0;
    bool freeze_db = false;
    bool json_out = false;
    std::string db_path = "../data/program_db.bin";
    std::string input_path;             // "" = stdin (only relevant if !inline_terms)
    bool inline_terms = false;          // true if --terms was given
    std::string inline_terms_csv;
    int inline_A = -1;
    std::string inline_id = "input";
    bool input_path_set = false;        // distinguishes "no path given" from "-" (stdin)

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help")            { printUsage(); return 0; }
        else if (a == "--budget"  && i + 1 < argc)      budget = atof(argv[++i]);
        else if (a == "--freeze-db")                    freeze_db = true;
        else if (a == "--db"      && i + 1 < argc)      db_path = argv[++i];
        else if (a == "--json")                         json_out = true;
        else if (a == "--terms"   && i + 1 < argc)      { inline_terms = true; inline_terms_csv = argv[++i]; }
        else if (a == "--A"       && i + 1 < argc)      inline_A = atoi(argv[++i]);
        else if (a == "--id"      && i + 1 < argc)      inline_id = argv[++i];
        else if (a == "-")                              { input_path.clear(); input_path_set = true; }
        else if (!a.empty() && a[0] != '-' && !input_path_set) { input_path = a; input_path_set = true; }
        else { fprintf(stderr, "omnis: unknown option '%s'\n", a.c_str()); printUsage(); return 2; }
    }

    // Read sequence.
    std::string id;
    int A = 0, N = 0;
    std::vector<int> tgt;

    if (inline_terms) {
        if (input_path_set) {
            fprintf(stderr, "omnis: cannot combine --terms with file/stdin input\n");
            return 2;
        }
        if (inline_A < 2) {
            fprintf(stderr, "omnis: --terms requires --A <n> with n >= 2\n");
            return 2;
        }
        // Parse CSV.
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
        if (N == 0) { fprintf(stderr, "omnis: --terms parsed no values\n"); return 2; }
    } else {
        int rc;
        if (input_path.empty()) {
            rc = readSequence(std::cin, id, A, N, tgt);
        } else {
            std::ifstream f(input_path);
            if (!f.is_open()) { fprintf(stderr, "omnis: cannot open '%s'\n", input_path.c_str()); return 3; }
            rc = readSequence(f, id, A, N, tgt);
        }
        if (rc != 0) return rc;
    }

    // Validate term range.
    for (int i = 0; i < N; i++) {
        if (tgt[i] < 0 || tgt[i] >= A) {
            fprintf(stderr, "omnis: term[%d]=%d out of [0, %d)\n", i, tgt[i], A);
            return 3;
        }
    }

    // Load program database.
    G_PROGDB_PATH = db_path.c_str();
    g_progdb = ProgramDB::load(db_path.c_str());

    // Compute ncat (canonical ISA + invocable library entries).
    int ncat = 0;
    { const auto& ranges = isaConstantRanges();
      for (auto& [t, cs] : ranges) ncat += (int)cs.size();
      int sz_lib = subCallCatalogSize();
      for (int idx = 0; idx < sz_lib; idx++)
          if (subCallLibraryEntryInvocable(idx)) ncat++;
    }

    // Solve.
    g_progs.clear();
    g_progs_fingerprints.clear();
    g_bench_prog_start = 0;
    double t0 = now_s();
    Res r = solve(tgt, A, now_s() + budget);
    double dt = now_s() - t0;
    r.mdl = computeMDL(r, ncat);
    bool solved = (r.sc == N);
    double raw_bits = N * log2(std::max(2, A));
    double ratio = (raw_bits > 0) ? r.mdl / raw_bits : 999.0;

    if (solved && !freeze_db) {
        g_progdb.add(r, ncat, tgt, A);
        g_progdb.save(db_path.c_str());
    }

    if (json_out) {
        printf("{\"id\":\"%s\",\"A\":%d,\"N\":%d,\"sc\":%d,\"solved\":%s,"
               "\"mdl\":%.2f,\"raw_bits\":%.2f,\"ratio\":%.4f,\"time_s\":%.3f,"
               "\"solver\":\"%s\"}\n",
               id.c_str(), A, N, r.sc, solved ? "true" : "false",
               r.mdl, raw_bits, ratio, dt, r.desc.c_str());
    } else {
        printf("%s\tsc=%d/%d\t%.1f%%\t%.3fs\tMDL=%.1f\traw=%.1f\tratio=%.4f\t%s\t%s\n",
               id.c_str(), r.sc, N, 100.0 * r.sc / std::max(1, N),
               dt, r.mdl, raw_bits, ratio,
               r.desc.c_str(), solved ? "SOLVED" : "PARTIAL");
    }

    return solved ? 0 : 1;
}
