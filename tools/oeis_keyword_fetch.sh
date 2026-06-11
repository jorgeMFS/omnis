#!/usr/bin/env bash
# oeis_keyword_fetch.sh - Build the frozen keywords.tsv snapshot.
#
# Queries the OEIS search API for each of {core, hard, base}, paginates
# through all results, parses the %K keyword line of each record, and
# writes data/oeis/snapshot/keywords.tsv (one line per A-number with full
# keyword set).
#
# DOCUMENTED OEIS API CONSTRAINTS (verified empirically 2026-05-06):
#   1. Page size capped at 100 records (n>100 silently truncates).
#   2. start offset capped at ~199 (start>=200 returns 0 records).
#   3. Broad keywords (easy, nonn) reject with "Too many results".
#
#   Net effect: at most ~200 records reachable per keyword query.
#
#   For our purposes:
#     core (183 total)   -> ALL reachable
#     hard (9558 total)  -> first 200 reachable
#     base (45411 total) -> first 200 reachable
#
#   Sequences outside these reachable ranges are not in keywords.tsv. The
#   loader therefore applies keyword constraints only when keyword data is
#   present; name-pattern filters work independently via names.gz.
#
# The output keywords.tsv is committed to the repo (small, ~few hundred KB)
# along with its SHA-256 sidecar. The loader (oeis_loader.cpp) reads it
# without re-querying OEIS, so the categorical pipeline runs offline once
# the snapshot is pinned.
#
# Usage:
#   tools/oeis_keyword_fetch.sh [--refresh]
#
# Exit codes:
#   0  keywords.tsv built/verified
#   1  network failure
#   2  prerequisite missing
#   3  pinned SHA mismatch (non-refresh mode and content changed)

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SNAPSHOT_DIR="$REPO_ROOT/data/oeis/snapshot"
OUT_TSV="$SNAPSHOT_DIR/keywords.tsv"
OUT_SHA="$SNAPSHOT_DIR/keywords.tsv.sha256"
SOURCES_MD="$REPO_ROOT/data/oeis/SOURCES.md"

REFRESH=0
[ "${1:-}" = "--refresh" ] && REFRESH=1

for cmd in curl shasum awk sort; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "oeis_keyword_fetch: missing prerequisite '$cmd'" >&2
        exit 2
    fi
done

mkdir -p "$SNAPSHOT_DIR"

UA="Mozilla/5.0 (compatible; omnis/0.1)"
PAGE_N=100        # OEIS hard-caps to 100 per page; requesting more silently truncates
MAX_START=100     # OEIS rejects start >= 200; last reachable page begins at 100
DELAY=1           # polite inter-request delay (seconds)

# Per-keyword query: paginate through all results, strip text format down
# to (%I id, %K keywords) pairs, append to accumulator.
fetch_keyword() {
    local kw="$1"
    local accum="$2"

    # Query first page to see the total
    local first
    first=$(curl -fsSL --max-time 60 -A "$UA" \
        "https://oeis.org/search?q=keyword:$kw&fmt=text&n=$PAGE_N&start=0" 2>/dev/null) \
        || { echo "oeis_keyword_fetch: query failed for keyword:$kw" >&2; return 1; }

    local total
    total=$(printf '%s\n' "$first" | grep -m 1 "^Showing" | awk '{print $NF}')
    if [ -z "$total" ]; then
        echo "oeis_keyword_fetch: keyword:$kw -> unable to parse total count" >&2
        echo "oeis_keyword_fetch: response head:" >&2
        printf '%s\n' "$first" | head -3 >&2
        return 1
    fi
    echo "oeis_keyword_fetch: keyword:$kw -> $total sequences"

    # Process first page
    printf '%s\n' "$first" | extract_pairs >> "$accum"

    # Pages 2..N up to OEIS's reachable limit (start <= MAX_START).
    local pages_total=$(( (total + PAGE_N - 1) / PAGE_N ))
    local reachable=$(( MAX_START / PAGE_N + 1 ))
    local pages_to_fetch=$(( pages_total < reachable ? pages_total : reachable ))

    if [ "$pages_total" -gt "$reachable" ]; then
        printf '  NOTE: keyword:%s has %d records; OEIS API exposes only %d (first %d). Capping.\n' \
            "$kw" "$total" "$reachable" $(( reachable * PAGE_N ))
    fi

    local p
    for (( p=1; p<pages_to_fetch; p++ )); do
        local start=$(( p * PAGE_N ))
        sleep "$DELAY"
        printf '  page %d/%d (start=%d)\n' $((p+1)) "$pages_to_fetch" "$start"
        curl -fsSL --max-time 60 -A "$UA" \
            "https://oeis.org/search?q=keyword:$kw&fmt=text&n=$PAGE_N&start=$start" 2>/dev/null \
            | extract_pairs >> "$accum"
    done
}

# Same shape as fetch_keyword but for free-text name searches. Used to
# capture keyword side-info for sequences that match our name-pattern
# filters (morphic, cellular automaton). The loader can then enforce
# nonn / non-sign constraints via this data.
fetch_name_query() {
    local query="$1"      # url-encoded query
    local label="$2"
    local accum="$3"

    local first
    first=$(curl -fsSL --max-time 60 -A "$UA" \
        "https://oeis.org/search?q=$query&fmt=text&n=$PAGE_N&start=0" 2>/dev/null) \
        || { echo "oeis_keyword_fetch: query failed for '$label'" >&2; return 1; }

    local total
    total=$(printf '%s\n' "$first" | grep -m 1 "^Showing" | awk '{print $NF}')
    [ -z "$total" ] && total=0
    echo "oeis_keyword_fetch: name '$label' -> $total sequences (capped at API limit)"

    printf '%s\n' "$first" | extract_pairs >> "$accum"

    local pages_total=$(( (total + PAGE_N - 1) / PAGE_N ))
    local reachable=$(( MAX_START / PAGE_N + 1 ))
    local pages_to_fetch=$(( pages_total < reachable ? pages_total : reachable ))

    local p
    for (( p=1; p<pages_to_fetch; p++ )); do
        local start=$(( p * PAGE_N ))
        sleep "$DELAY"
        printf '  page %d/%d (start=%d)\n' $((p+1)) "$pages_to_fetch" "$start"
        curl -fsSL --max-time 60 -A "$UA" \
            "https://oeis.org/search?q=$query&fmt=text&n=$PAGE_N&start=$start" 2>/dev/null \
            | extract_pairs >> "$accum"
    done
}

# Read OEIS text format on stdin, emit "<A-number><TAB><keyword,csv>" pairs.
# Note: the %K line lists keywords for the matching record, including the
# canonical "nonn"/"sign" classification - we get that side-info free.
extract_pairs() {
    awk '
        /^%I A[0-9]/ { id = $2 }
        /^%K A[0-9]/ {
            if ($2 != id) next
            $1 = ""; $2 = ""
            sub(/^[ \t]+/, "")
            kws = $0
            print id "\t" kws
            id = ""
        }
    '
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TMP_ACCUM=$(mktemp)
trap 'rm -f "$TMP_ACCUM"' EXIT

now_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ)

for kw in core hard base; do
    fetch_keyword "$kw" "$TMP_ACCUM"
    sleep "$DELAY"
done

# Also fetch keyword side-info for name-pattern filter targets, so the
# loader can enforce nonn/non-sign on morphic and cellular categories.
fetch_name_query "morphic"               "morphic"            "$TMP_ACCUM"
sleep "$DELAY"
fetch_name_query "cellular+automaton"    "cellular automaton" "$TMP_ACCUM"

# Deduplicate the body. A given sequence may appear under multiple queries;
# the %K line is canonical so duplicates should be byte-equal. `sort -u`
# handles dedup + deterministic ordering.
TMP_BODY=$(mktemp)
trap 'rm -f "$TMP_ACCUM" "$TMP_BODY"' EXIT
sort -u "$TMP_ACCUM" > "$TMP_BODY"

# Compute body-only SHA - invariant across runs of identical OEIS state.
body_sha=$(shasum -a 256 "$TMP_BODY" | cut -d' ' -f1)

# Compose the final file: headers (incl. body_sha256) + body. The wall-clock
# timestamp in the header is informational only; reproducibility lives in
# body_sha256 - that is what we pin and verify.
{
    echo "# omnis-oeis-keywords v1"
    echo "# fetched_utc: $now_iso"
    echo "# source_queries: keyword:core, keyword:hard, keyword:base, name:morphic, name:cellular+automaton"
    echo "# api_limit_note: OEIS search API caps at ~200 records per query; coverage is first-N for queries that exceed this"
    echo "# body_sha256: $body_sha"
    echo "# format: <a_number>\\t<comma-separated keyword set>"
    cat "$TMP_BODY"
} > "$OUT_TSV"

# Compare body SHA against committed pin (this is the reproducibility contract).
old_body_sha=""
[ -f "$OUT_SHA" ] && old_body_sha=$(awk '{print $1}' "$OUT_SHA")

if [ -n "$old_body_sha" ] && [ "$REFRESH" = "0" ] && [ "$body_sha" != "$old_body_sha" ]; then
    echo "oeis_keyword_fetch: BODY SHA mismatch  pinned=$old_body_sha  current=$body_sha" >&2
    echo "  Upstream OEIS keyword data has drifted since pin."     >&2
    echo "  Re-pin with 'tools/oeis_keyword_fetch.sh --refresh' if intended." >&2
    exit 3
fi

# Sidecar carries the body SHA, not the wall-clock-perturbed file SHA.
printf '%s  keywords.tsv (body)\n' "$body_sha" > "$OUT_SHA"

new_sha="$body_sha"  # for downstream "wrote ... sha=" line

# Append entry to SOURCES.md (idempotent: replace existing entry if present)
if [ -f "$SOURCES_MD" ]; then
    # Strip any prior keywords.tsv block, then append the new one
    awk '
        /^- `data\/oeis\/snapshot\/keywords.tsv`/ { skip = 1; next }
        skip && /^- `/                          { skip = 0 }
        skip && /^[[:space:]]*$/                { skip = 0 }
        !skip                                    { print }
    ' "$SOURCES_MD" > "$SOURCES_MD.tmp" && mv "$SOURCES_MD.tmp" "$SOURCES_MD"
    {
        echo "- \`data/oeis/snapshot/keywords.tsv\`"
        echo "    derivation:  search keyword:{core,hard,base}, paginated, %K extracted"
        echo "    fetched_utc: $now_iso"
        echo "    size_bytes:  $(stat -f %z "$OUT_TSV" 2>/dev/null || stat -c %s "$OUT_TSV")"
        echo "    sha256:      $new_sha"
        echo ""
    } >> "$SOURCES_MD"
fi

records=$(grep -c "^A" "$OUT_TSV" || true)
echo "oeis_keyword_fetch: wrote $OUT_TSV ($records records, sha=$new_sha)"
