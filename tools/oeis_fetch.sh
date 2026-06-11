#!/usr/bin/env bash
# oeis_fetch.sh — Pin a reproducible OEIS snapshot.
#
# Downloads:
#   - stripped.gz  (numerical sequences, ~31 MB)
#   - names.gz     (sequence names, ~7.5 MB)
#   - canary b-files for cross-validation
#
# Behaviour:
#   - On first run: downloads, computes SHA-256, writes data/oeis/SOURCES.md.
#   - On subsequent runs: redownloads, verifies SHA matches the committed pin,
#     fails fast if OEIS has updated since pinning.
#
#   The bulk files (stripped.gz, names.gz) are NOT committed to git — they are
#   30+ MB and recreatable from the pinned SHA. The pinned SHA + this script
#   ARE the reproducibility contract.
#
# Usage:
#   tools/oeis_fetch.sh [--refresh]   # --refresh: rewrite the pin from current upstream
#
# Exit codes:
#   0  snapshot present and verified
#   1  download failed
#   2  SHA mismatch (OEIS upstream has changed since pin) — see SOURCES.md
#   3  prerequisite missing (curl, shasum)

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SNAPSHOT_DIR="$REPO_ROOT/data/oeis/snapshot"
CANARY_DIR="$REPO_ROOT/data/oeis/canary"
SOURCES_MD="$REPO_ROOT/data/oeis/SOURCES.md"

REFRESH=0
[ "${1:-}" = "--refresh" ] && REFRESH=1

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------

for cmd in curl shasum gunzip; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "oeis_fetch: missing prerequisite '$cmd'" >&2
        exit 3
    fi
done

mkdir -p "$SNAPSHOT_DIR" "$CANARY_DIR"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

sha256_of() {
    shasum -a 256 "$1" | cut -d' ' -f1
}

http_get() {
    # $1 = URL, $2 = output path
    # Robust: 5 retries, exponential backoff via --retry, max 120s per attempt.
    # Plain UA — oeis.org's WAF rejects some custom strings; a vanilla browser
    # UA is the documented contract for the bulk download endpoints.
    curl -fsSL --retry 5 --retry-delay 2 --max-time 120 \
         -A "Mozilla/5.0 (compatible; omnis/0.1)" \
         -o "$2" "$1"
}

# Read the pinned SHA for a given file from SOURCES.md, or empty if not pinned.
pinned_sha() {
    local path_rel="$1"
    [ -f "$SOURCES_MD" ] || { echo ""; return; }
    awk -v p="$path_rel" '
        $0 ~ "^- `" p "`" { found=1; next }
        found && /sha256:/ { print $2; exit }
    ' "$SOURCES_MD"
}

# ---------------------------------------------------------------------------
# Bulk files: stripped.gz, names.gz
# ---------------------------------------------------------------------------

declare -a BULK_FILES=(
    "stripped.gz https://oeis.org/stripped.gz"
    "names.gz    https://oeis.org/names.gz"
)

# Canary b-files (per-sequence authoritative term lists for cross-validation).
declare -a CANARY_FILES=(
    "b000005.txt https://oeis.org/A000005/b000005.txt"
    "b000010.txt https://oeis.org/A000010/b000010.txt"
    "b000040.txt https://oeis.org/A000040/b000040.txt"
    "b007814.txt https://oeis.org/A007814/b007814.txt"
    "b005132.txt https://oeis.org/A005132/b005132.txt"
)

now_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ)
declare -a NEW_LINES=()

# ---- Bulk
for entry in "${BULK_FILES[@]}"; do
    fname=$(echo "$entry" | awk '{print $1}')
    furl=$(echo "$entry" | awk '{print $2}')
    out="$SNAPSHOT_DIR/$fname"
    rel="data/oeis/snapshot/$fname"

    echo "oeis_fetch: $furl -> $rel"
    http_get "$furl" "$out" || { echo "  download failed" >&2; exit 1; }

    new_sha=$(sha256_of "$out")
    pinned=$(pinned_sha "$rel")
    size=$(stat -f %z "$out" 2>/dev/null || stat -c %s "$out")

    if [ -n "$pinned" ] && [ "$REFRESH" = "0" ] && [ "$new_sha" != "$pinned" ]; then
        echo "  SHA MISMATCH  pinned=$pinned  current=$new_sha" >&2
        echo "  Upstream OEIS has updated since the pinned snapshot." >&2
        echo "  Either: (a) check out an omnis revision matching the upstream snapshot," >&2
        echo "          (b) re-pin with 'tools/oeis_fetch.sh --refresh' (then re-build keywords)." >&2
        exit 2
    fi

    NEW_LINES+=("- \`$rel\`")
    NEW_LINES+=("    url:        $furl")
    NEW_LINES+=("    fetched_utc: $now_iso")
    NEW_LINES+=("    size_bytes: $size")
    NEW_LINES+=("    sha256:     $new_sha")
    NEW_LINES+=("")
done

# ---- Canary
for entry in "${CANARY_FILES[@]}"; do
    fname=$(echo "$entry" | awk '{print $1}')
    furl=$(echo "$entry" | awk '{print $2}')
    out="$CANARY_DIR/$fname"
    rel="data/oeis/canary/$fname"

    echo "oeis_fetch: $furl -> $rel"
    http_get "$furl" "$out" || { echo "  download failed" >&2; exit 1; }

    new_sha=$(sha256_of "$out")
    pinned=$(pinned_sha "$rel")
    size=$(stat -f %z "$out" 2>/dev/null || stat -c %s "$out")

    if [ -n "$pinned" ] && [ "$REFRESH" = "0" ] && [ "$new_sha" != "$pinned" ]; then
        echo "  CANARY SHA MISMATCH  pinned=$pinned  current=$new_sha" >&2
        echo "  Either OEIS revised this b-file or it was extended; re-pin with --refresh." >&2
        exit 2
    fi

    NEW_LINES+=("- \`$rel\`")
    NEW_LINES+=("    url:        $furl")
    NEW_LINES+=("    fetched_utc: $now_iso")
    NEW_LINES+=("    size_bytes: $size")
    NEW_LINES+=("    sha256:     $new_sha")
    NEW_LINES+=("")
done

# ---------------------------------------------------------------------------
# Write / refresh SOURCES.md
# ---------------------------------------------------------------------------

if [ "$REFRESH" = "1" ] || [ ! -f "$SOURCES_MD" ]; then
    {
        echo "# OEIS snapshot — pinned sources"
        echo ""
        echo "Pinned: $now_iso"
        echo ""
        echo "Reproducibility contract: anyone running \`tools/oeis_fetch.sh\` re-derives these"
        echo "files. If upstream OEIS has updated, the SHA verification fails fast and the"
        echo "user is asked to either check out a matching revision of omnis or re-pin via"
        echo "\`tools/oeis_fetch.sh --refresh\`."
        echo ""
        echo "## Bulk"
        echo ""
        for line in "${NEW_LINES[@]}"; do
            echo "$line"
        done
    } > "$SOURCES_MD"
    echo "oeis_fetch: rewrote $SOURCES_MD"
else
    echo "oeis_fetch: SOURCES.md unchanged (existing pins verified)"
fi

# Also emit individual sha256 sidecar files so external tools (CI) can verify
# without parsing markdown.
for entry in "${BULK_FILES[@]}"; do
    fname=$(echo "$entry" | awk '{print $1}')
    sha=$(sha256_of "$SNAPSHOT_DIR/$fname")
    printf "%s  %s\n" "$sha" "$fname" > "$SNAPSHOT_DIR/$fname.sha256"
done
for entry in "${CANARY_FILES[@]}"; do
    fname=$(echo "$entry" | awk '{print $1}')
    sha=$(sha256_of "$CANARY_DIR/$fname")
    printf "%s  %s\n" "$sha" "$fname" > "$CANARY_DIR/$fname.sha256"
done

echo "oeis_fetch: snapshot ready under $SNAPSHOT_DIR + $CANARY_DIR"
