// oeis_loader.cpp — Streaming OEIS workload emitter.
//
// Pure C++17. No engine link.
//
// Reads:
//   data/oeis/snapshot/stripped.gz     (numerical sequences)
//   data/oeis/snapshot/names.gz        (sequence names)
//   data/oeis/snapshot/keywords.tsv    (frozen keyword side-info)
//
// Applies a named filter (see oeis_filters[]), reduces each matched
// sequence mod each target alphabet, truncates to N terms, and emits
// in the omnis-workload v1 format on stdout.
//
// Big-int safety: terms are parsed as strings and reduced mod A
// digit-by-digit so sequences with arbitrarily large terms (well past
// int64 range) are still handled losslessly. Negative terms are reduced
// via Euclidean modulus.
//
// Determinism: same (snapshot SHAs, filter, target_n, alphabets)
// produces byte-identical body. Wall-clock timestamp lives in the header
// only.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

// =============================================================================
// Loader versioning — bump if any filter's output for fixed snapshot changes.
// =============================================================================

static constexpr int LOADER_VERSION = 1;
static constexpr const char* WORKLOAD_SCHEMA = "omnis-workload v1";

// =============================================================================
// Filter registry — pre-registered, frozen.
// =============================================================================

struct OeisFilter {
    std::string name;                        // category id used in workload header
    std::vector<std::string> required_kws;   // ALL must be present in keyword set
    std::vector<std::string> excluded_kws;   // NONE may be present
    std::vector<std::string> name_substrs;   // case-insensitive; ANY substring match qualifies (OR)
    std::vector<int> alphabets;              // emit one row per alphabet
    int target_n;                            // ideal sequence length; truncates if longer
    int min_n;                               // minimum acceptable; reject sequences shorter
    int max_count;                           // 0 = no cap; otherwise stop at this many sequences
    std::string description;
};

// stripped.gz typically gives 30–130 terms per sequence; fast-growing
// sequences (Bell, Fibonacci-large-mod, etc.) are truncated below that.
// min_n=20 captures ~91% of keyword:core; lower bound chosen so binary
// alphabet emissions still carry ≥20 bits = enough for trivial-pattern
// detection. target_n=500 truncates the long tail. For runs
// that need longer sequences, run tools/oeis_bfile_fetch.sh
// to extend coverage from authoritative per-sequence b-files.

static const std::vector<OeisFilter> FILTERS = {
    {
        "oeis_core",
        {"core", "nonn"},
        {"sign", "fini"},
        {},
        {2, 3, 4, 5, 7},
        500, 20, 0,
        "Sloane's curated core (keyword:core), alphabets {2,3,4,5,7}, N up to 500"
    },
    {
        "oeis_hard",
        {"hard", "nonn"},
        {"sign", "fini"},
        {},
        {2, 3, 4},
        500, 20, 0,
        "Hard sequences (keyword:hard), first ~200 reachable via OEIS API, N up to 500"
    },
    {
        "oeis_base",
        {"base", "nonn"},
        {"sign", "fini"},
        {},
        {2, 3, 4},
        500, 20, 0,
        "Base/digit-related (keyword:base), first ~200 reachable via OEIS API, N up to 500"
    },
    {
        "oeis_morphic",
        {"nonn"},
        {"sign", "fini"},
        {"morphic"},
        {2, 3},
        500, 20, 0,
        "Sequences with 'morphic' in name; nonn enforced where keyword data available"
    },
    {
        "oeis_cellular",
        {"nonn"},
        {"sign", "fini"},
        {"cellular automaton", "wolfram", "rule "},
        {2, 3},
        500, 20, 0,
        "Sequences with cellular-automaton-related names; first ~200 reachable"
    },
};

static const OeisFilter* findFilter(const std::string& name) {
    for (const auto& f : FILTERS) if (f.name == name) return &f;
    return nullptr;
}

// =============================================================================
// SHA-256 (same self-contained impl as gen_workload.cpp)
// =============================================================================

namespace sha256_impl {
    using u32 = uint32_t; using u64 = uint64_t;
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
        u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
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
// Big-integer string-modulus (handles arbitrarily large OEIS terms)
// =============================================================================

// Compute |s| mod m for decimal string s (digits only, no sign).
// O(len(s)) — process one digit at a time.
static int absStrMod(const char* s, size_t len, int m) {
    int r = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') continue;
        r = (int)(((int64_t)r * 10 + (s[i] - '0')) % m);
    }
    return r;
}

// Reduce a signed decimal string mod m using Euclidean modulus.
static int signedStrMod(const std::string& tok, int m) {
    if (tok.empty()) return 0;
    bool neg = (tok[0] == '-');
    size_t off = (tok[0] == '-' || tok[0] == '+') ? 1 : 0;
    int r = absStrMod(tok.data() + off, tok.size() - off, m);
    if (neg) r = (m - r) % m;
    return ((r % m) + m) % m;
}

// =============================================================================
// gzip line streaming
// =============================================================================

struct GzLineReader {
    gzFile f = nullptr;
    std::string buf;
    bool eof = false;

    bool open(const std::string& path) {
        f = gzopen(path.c_str(), "rb");
        return f != nullptr;
    }

    ~GzLineReader() { if (f) gzclose(f); }

    bool nextLine(std::string& line) {
        line.clear();
        if (!f) return false;
        char ch[16384];
        // Drain anything already in the buffer.
        if (!buf.empty()) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                line.assign(buf, 0, nl);
                buf.erase(0, nl + 1);
                return true;
            } else {
                line = buf;
                buf.clear();
            }
        }
        while (true) {
            int n = gzread(f, ch, sizeof(ch));
            if (n <= 0) {
                eof = true;
                return !line.empty();
            }
            for (int i = 0; i < n; i++) {
                if (ch[i] == '\n') {
                    buf.assign(ch + i + 1, n - i - 1);
                    return true;
                } else {
                    line.push_back(ch[i]);
                }
            }
        }
    }
};

// =============================================================================
// Snapshot loaders
// =============================================================================

// keywords.tsv: "Annnnnnn<TAB>k1,k2,..."
// '#' lines are headers/comments. Returns map A-number -> set of keywords.
static std::unordered_map<std::string, std::unordered_set<std::string>>
loadKeywords(const std::string& path) {
    std::unordered_map<std::string, std::unordered_set<std::string>> m;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::fprintf(stderr, "oeis_loader: cannot open '%s'\n", path.c_str());
        return m;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string id = line.substr(0, tab);
        std::string kws = line.substr(tab + 1);
        std::unordered_set<std::string> kset;
        size_t pos = 0;
        while (pos <= kws.size()) {
            size_t comma = kws.find(',', pos);
            std::string k = kws.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            // trim whitespace
            while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
            while (!k.empty() && std::isspace((unsigned char)k.front())) k.erase(0, 1);
            if (!k.empty()) kset.insert(k);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        m[id] = std::move(kset);
    }
    return m;
}

// names.gz: "Annnnnnn Name of the sequence ..."
static std::unordered_map<std::string, std::string>
loadNames(const std::string& path) {
    std::unordered_map<std::string, std::string> m;
    GzLineReader gr;
    if (!gr.open(path)) {
        std::fprintf(stderr, "oeis_loader: cannot open '%s'\n", path.c_str());
        return m;
    }
    std::string line;
    while (gr.nextLine(line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] != 'A') continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string id = line.substr(0, sp);
        std::string name = line.substr(sp + 1);
        m[id] = std::move(name);
    }
    return m;
}

// Lowercase-substring match (predicate for name patterns).
static bool nameContainsAny(const std::string& name,
                            const std::vector<std::string>& subs) {
    if (subs.empty()) return true;  // no name constraint
    std::string lo = name;
    for (auto& c : lo) c = std::tolower((unsigned char)c);
    for (const auto& s : subs) {
        std::string sl = s;
        for (auto& c : sl) c = std::tolower((unsigned char)c);
        if (lo.find(sl) != std::string::npos) return true;
    }
    return false;
}

// Keyword-set predicate: ALL required must be present, NONE excluded.
// If keyword set is empty (sequence not in keywords.tsv), apply
// "permissive on missing" — required_kws cannot be enforced; excluded_kws
// also cannot be detected. Caller distinguishes via keyword_known.
static bool keywordsOk(const std::unordered_set<std::string>* kws,
                       const std::vector<std::string>& required,
                       const std::vector<std::string>& excluded) {
    if (!kws) {
        // No keyword data — only honor name-based filtering. Return true to
        // allow caller's name match to be the deciding factor.
        return true;
    }
    for (const auto& k : required) {
        if (kws->find(k) == kws->end()) return false;
    }
    for (const auto& k : excluded) {
        if (kws->find(k) != kws->end()) return false;
    }
    return true;
}

// =============================================================================
// stripped.gz parser
// =============================================================================
//
// Format (per record):
//   Annnnnnn ,t0,t1,t2,...
//
// First line is a comment. Some sequences include negative terms ("-").

struct OeisRecord {
    std::string id;
    std::vector<std::string> term_strings;  // raw decimal strings; reduced lazily
};

static bool parseRecord(const std::string& line, OeisRecord& r) {
    if (line.empty() || line[0] != 'A') return false;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) return false;
    r.id = line.substr(0, sp);
    r.term_strings.clear();

    // Term list: " ,t0,t1,..." or ",t0,t1,..."
    size_t p = sp;
    while (p < line.size() && (line[p] == ' ' || line[p] == ',')) p++;
    while (p < line.size()) {
        size_t comma = line.find(',', p);
        size_t end = (comma == std::string::npos) ? line.size() : comma;
        if (end > p) {
            std::string tok(line.data() + p, end - p);
            // trim trailing whitespace
            while (!tok.empty() && std::isspace((unsigned char)tok.back())) tok.pop_back();
            if (!tok.empty()) r.term_strings.push_back(std::move(tok));
        }
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    return true;
}

// =============================================================================
// Workload emission
// =============================================================================

struct Emit {
    std::string id;
    int A;
    std::vector<int> terms;
};

static std::string formatLine(const Emit& e) {
    std::ostringstream oss;
    oss << e.id << ' ' << e.A << ' ' << e.terms.size();
    for (int t : e.terms) oss << ' ' << t;
    return oss.str();
}

static std::string isoDateUtc() {
    const char* sde = std::getenv("SOURCE_DATE_EPOCH");
    time_t now = sde ? (time_t)std::strtoll(sde, nullptr, 10) : std::time(nullptr);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// =============================================================================
// CLI
// =============================================================================

static void printUsage() {
    std::fprintf(stderr,
        "oeis_loader — emit OMNIS workloads sourced from the pinned OEIS snapshot.\n"
        "\n"
        "Usage:\n"
        "  oeis_loader --filter <name> [--snapshot-dir DIR] [--out PATH]\n"
        "             [--snapshot-sha SHA] [--max N]\n"
        "\n"
        "Filters (pre-registered in oeis_loader.cpp):\n"
        "  oeis_core, oeis_hard, oeis_base, oeis_morphic, oeis_cellular\n"
        "\n"
        "Options:\n"
        "  --filter F             named filter (required)\n"
        "  --snapshot-dir DIR     directory containing stripped.gz, names.gz, keywords.tsv\n"
        "                         (default: data/oeis/snapshot relative to CWD)\n"
        "  --out PATH             output file (default: stdout)\n"
        
        "  --snapshot-sha SHA     recorded in workload header (use stripped.gz body sha)\n"
        "  --max N                cap to first N matching sequences (0 = filter default)\n"
        "  --list-filters         print filter table and exit\n"
    );
}

int main(int argc, char* argv[]) {
    std::string filter_name;
    std::string snapshot_dir = "data/oeis/snapshot";
    std::string out_path;
    std::string snapshot_sha;
    int max_override = -1;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") { printUsage(); return 0; }
        else if (a == "--list-filters") {
            std::printf("%-18s %-8s  %s\n", "name", "alphabets", "description");
            std::printf("%s\n", std::string(80, '-').c_str());
            for (const auto& f : FILTERS) {
                std::ostringstream a_str;
                for (size_t i = 0; i < f.alphabets.size(); i++) {
                    if (i) a_str << ',';
                    a_str << f.alphabets[i];
                }
                std::printf("%-18s %-8s  %s\n", f.name.c_str(), a_str.str().c_str(), f.description.c_str());
            }
            return 0;
        }
        else if (a == "--filter"        && i + 1 < argc) filter_name = argv[++i];
        else if (a == "--snapshot-dir"  && i + 1 < argc) snapshot_dir = argv[++i];
        else if (a == "--out"           && i + 1 < argc) out_path = argv[++i];
            else if (a == "--snapshot-sha"  && i + 1 < argc) snapshot_sha = argv[++i];
        else if (a == "--max"           && i + 1 < argc) max_override = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "oeis_loader: unknown option '%s'\n", a.c_str()); printUsage(); return 2; }
    }

    if (filter_name.empty()) {
        std::fprintf(stderr, "oeis_loader: --filter is required\n");
        printUsage();
        return 2;
    }
    const OeisFilter* filt = findFilter(filter_name);
    if (!filt) {
        std::fprintf(stderr, "oeis_loader: unknown filter '%s' (try --list-filters)\n", filter_name.c_str());
        return 2;
    }

    // Load snapshot side-info.
    std::string keywords_path = snapshot_dir + "/keywords.tsv";
    std::string names_path    = snapshot_dir + "/names.gz";
    std::string stripped_path = snapshot_dir + "/stripped.gz";

    auto kw_map   = loadKeywords(keywords_path);
    auto name_map = loadNames(names_path);
    if (kw_map.empty() && name_map.empty()) {
        std::fprintf(stderr, "oeis_loader: snapshot directory '%s' yielded no usable data\n",
                     snapshot_dir.c_str());
        return 3;
    }
    std::fprintf(stderr, "oeis_loader: keywords=%zu names=%zu\n", kw_map.size(), name_map.size());

    // First pass over stripped.gz: select sequences that pass the filter,
    // build emissions per alphabet.
    GzLineReader gr;
    if (!gr.open(stripped_path)) {
        std::fprintf(stderr, "oeis_loader: cannot open '%s'\n", stripped_path.c_str());
        return 3;
    }

    int cap = (max_override > 0) ? max_override
            : (filt->max_count > 0 ? filt->max_count : INT32_MAX);

    std::vector<Emit> emissions;
    std::string line;
    int matched_seqs = 0;

    while (gr.nextLine(line)) {
        if (line.empty() || line[0] == '#') continue;

        OeisRecord r;
        if (!parseRecord(line, r)) continue;

        // Keyword check (returns true if no keyword data for this id, leaving
        // name-based filtering as the deciding signal).
        const std::unordered_set<std::string>* kws = nullptr;
        auto kit = kw_map.find(r.id);
        if (kit != kw_map.end()) kws = &kit->second;
        if (!keywordsOk(kws, filt->required_kws, filt->excluded_kws)) continue;

        // For name-pattern filters, reject when there is no name match.
        // For keyword-only filters (no name_substrs), this is auto-true.
        if (!filt->name_substrs.empty()) {
            auto nit = name_map.find(r.id);
            if (nit == name_map.end()) continue;
            if (!nameContainsAny(nit->second, filt->name_substrs)) continue;
            // Also: if filter has required keywords AND we have no keyword
            // data for this id, refuse — we can't claim the constraint holds.
            if (!kws && !filt->required_kws.empty()) continue;
        } else {
            // Pure keyword filter: must have keyword data and pass.
            if (!kws) continue;
        }

        // Length check: skip sequences shorter than the configured floor.
        if ((int)r.term_strings.size() < filt->min_n) continue;

        // Emit per alphabet.
        for (int A : filt->alphabets) {
            int N = std::min(filt->target_n, (int)r.term_strings.size());
            if (N <= 0) continue;
            std::vector<int> ts;
            ts.reserve(N);
            for (int i = 0; i < N; i++) {
                ts.push_back(signedStrMod(r.term_strings[i], A));
            }
            char id[64];
            std::snprintf(id, sizeof(id), "%s_a%d", r.id.c_str(), A);
            emissions.push_back({id, A, std::move(ts)});
        }
        matched_seqs++;
        if (matched_seqs >= cap) break;
    }

    std::fprintf(stderr, "oeis_loader: filter='%s' matched=%d sequences -> %zu emissions (across %zu alphabets)\n",
                 filt->name.c_str(), matched_seqs, emissions.size(), filt->alphabets.size());

    // Compose output.
    std::ostringstream body_oss;
    for (const auto& e : emissions) body_oss << formatLine(e) << '\n';
    std::string body = body_oss.str();
    std::string body_sha = sha256_impl::hex(body);

    std::ofstream ofs;
    std::ostream* out = &std::cout;
    if (!out_path.empty()) {
        ofs.open(out_path);
        if (!ofs.is_open()) { std::fprintf(stderr, "oeis_loader: cannot write '%s'\n", out_path.c_str()); return 3; }
        out = &ofs;
    }

    *out << "# " << WORKLOAD_SCHEMA << '\n';
    *out << "# category: " << filt->name << '\n';
    *out << "# loader_version: " << LOADER_VERSION << '\n';
    *out << "# oeis_snapshot_sha: " << (snapshot_sha.empty() ? "unknown" : snapshot_sha) << '\n';
    *out << "# omnis_min_version: 0.1.0\n";
    *out << "# created_utc: " << isoDateUtc() << '\n';
    *out << "# matched_sequences: " << matched_seqs << '\n';
    *out << "# count: " << emissions.size() << '\n';
    *out << "# body_sha256: " << body_sha << '\n';
    *out << "# selection_rule: " << filt->description << '\n';
    *out << body;

    return 0;
}
