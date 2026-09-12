#!/usr/bin/env bash
# Fail if a doctest case present in the source-layout suite is missing from
# the dist-layout suite, unless the name is listed in test/dist-exempt.txt.
#
# Protocol tests used to be filtered out of `CSP_INCLUDE=dist` (Makefile),
# so ASan/TSan/UBSan never ran HTTP/WS/QUIC against the amalgamated header.
# This script is the ratchet: a case name disappearing from dist without an
# explicit exemption is a failed oracle, not a silent skip.
#
# Usage: scripts/check_dist_test_parity.sh
# Env:   INCLUDE_BIN  default build/normal/csp_tests
#        DIST_BIN     default build/normal-dist/csp_tests

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE_BIN="${INCLUDE_BIN:-$ROOT/build/normal/csp_tests}"
DIST_BIN="${DIST_BIN:-$ROOT/build/normal-dist/csp_tests}"
EXEMPT="$ROOT/test/dist-exempt.txt"

PROTOCOL_SRCS="
$ROOT/test/net.test.cc
$ROOT/test/http.test.cc
$ROOT/test/http2.test.cc
$ROOT/test/http3.test.cc
$ROOT/test/ws.test.cc
$ROOT/test/quic.test.cc
"

die() { echo "check_dist_test_parity: $*" >&2; exit 1; }

list_from_bin() {
    local bin="$1"
    # doctest prints a banner, then one case name per line (duplicates kept:
    # http and http2 both have serve---basic-get).
    "$bin" --list-test-cases --no-colors 2>/dev/null \
        | grep -v '^\[' | grep -v '^=' | grep -v '^$' || true
}

list_from_sources() {
    local f
    for f in $PROTOCOL_SRCS; do
        [ -f "$f" ] || continue
        sed -n 's/.*TEST_CASE("\([^"]*\)").*/\1/p' "$f"
    done
}

load_exempt() {
    [ -f "$EXEMPT" ] || return 0
    grep -v '^[[:space:]]*#' "$EXEMPT" | grep -v '^[[:space:]]*$' || true
}

TMPDIR_LIST="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LIST"' EXIT

if [ ! -x "$DIST_BIN" ]; then
    die "dist binary missing: $DIST_BIN (build with: make CSP_INCLUDE=dist CSP_TLS=1 build)"
fi

list_from_bin "$DIST_BIN" | sort > "$TMPDIR_LIST/dist"

# Expected protocol names always come from the six source files, so the
# check is meaningful even when the include-layout binary is absent (CI
# sanitize job builds only dist).
list_from_sources | sort > "$TMPDIR_LIST/protocol"
if [ ! -s "$TMPDIR_LIST/protocol" ]; then
    die "no TEST_CASE names found in protocol test sources"
fi

missing_protocol="$(comm -23 "$TMPDIR_LIST/protocol" "$TMPDIR_LIST/dist" || true)"
if [ -n "$missing_protocol" ]; then
    echo "check_dist_test_parity: protocol cases missing from dist listing:" >&2
    echo "$missing_protocol" | sed 's/^/  /' >&2
    exit 1
fi

if [ -x "$INCLUDE_BIN" ]; then
    list_from_bin "$INCLUDE_BIN" | sort > "$TMPDIR_LIST/include"
    load_exempt | sort -u > "$TMPDIR_LIST/exempt"
    # include minus exempt must be a submultiset of dist.
    if [ -s "$TMPDIR_LIST/exempt" ]; then
        comm -23 "$TMPDIR_LIST/include" "$TMPDIR_LIST/exempt" > "$TMPDIR_LIST/include_kept"
    else
        cp "$TMPDIR_LIST/include" "$TMPDIR_LIST/include_kept"
    fi
    missing_include="$(comm -23 "$TMPDIR_LIST/include_kept" "$TMPDIR_LIST/dist" || true)"
    if [ -n "$missing_include" ]; then
        echo "check_dist_test_parity: source-layout cases missing from dist (not in dist-exempt.txt):" >&2
        echo "$missing_include" | sed 's/^/  /' >&2
        exit 1
    fi
fi

echo "check_dist_test_parity: ok ($(wc -l < "$TMPDIR_LIST/protocol" | tr -d ' ') protocol cases present in dist)"
