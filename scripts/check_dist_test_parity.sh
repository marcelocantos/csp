#!/usr/bin/env bash
# Compare complete, configuration-matched source and dist doctest inventories,
# accounting for the white-box cases listed in test/dist-exempt.txt.
#
# Protocol tests used to be filtered out of `CSP_INCLUDE=dist` (Makefile),
# so ASan/TSan/UBSan never ran HTTP/WS/QUIC against the amalgamated header.
# This script is the ratchet: a case name disappearing from dist without an
# explicit exemption is a failed oracle, not a silent skip.
#
# Usage: make check-dist-test-parity [CSP_TLS=0] [SANITIZE=...]
# Direct use requires both binaries built with the same feature/sanitizer flags.
# Env:   INCLUDE_BIN  default build/normal/csp_tests
#        DIST_BIN     default build/normal-dist/csp_tests

set -euo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE_BIN="${INCLUDE_BIN:-$ROOT/build/normal/csp_tests}"
DIST_BIN="${DIST_BIN:-$ROOT/build/normal-dist/csp_tests}"
EXEMPT="$ROOT/test/dist-exempt.txt"

die() { echo "check_dist_test_parity: $*" >&2; exit 1; }

list_from_bin() {
    local bin="$1"
    local output="$2"
    [[ -x "$bin" ]] || die "test binary missing: $bin (run make check-dist-test-parity)"
    # Capture the process separately: a failed/crashing listing must not turn
    # into an empty successful inventory. Keep stderr visible for diagnostics.
    "$bin" --list-test-cases --no-colors > "$output.raw"
    # The vendored doctest console listing has two delimiter lines. Preserve
    # duplicate names (different suites may use the same name), and fail closed
    # on an empty or malformed listing instead of accepting arbitrary output.
    if ! awk '
        /^=+$/ { section++; next }
        section == 1 { print; count++ }
        /^\[doctest\] unskipped test cases passing the current filters: [0-9]+$/ {
            reported = $NF; summary++
        }
        END { if (section != 2 || summary != 1 || count == 0 || count != reported) exit 1 }
    ' "$output.raw" > "$output.unsorted"; then
        die "empty or malformed doctest listing from $bin"
    fi
    sort "$output.unsorted" > "$output"
}

TMPDIR_LIST="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LIST"' EXIT

[[ ! "$INCLUDE_BIN" -ef "$DIST_BIN" ]] || die "source and dist paths refer to the same binary"
list_from_bin "$INCLUDE_BIN" "$TMPDIR_LIST/include"
list_from_bin "$DIST_BIN" "$TMPDIR_LIST/dist"
awk '!/^[[:space:]]*(#|$)/' "$EXEMPT" > "$TMPDIR_LIST/exempt.unsorted"
sort -u "$TMPDIR_LIST/exempt.unsorted" > "$TMPDIR_LIST/exempt"
comm -23 "$TMPDIR_LIST/include" "$TMPDIR_LIST/exempt" > "$TMPDIR_LIST/include_kept"
if ! diff -u "$TMPDIR_LIST/include_kept" "$TMPDIR_LIST/dist"; then
    die "source and dist test inventories differ (after dist-exempt.txt); check build flags and coverage"
fi

echo "check_dist_test_parity: ok ($(wc -l < "$TMPDIR_LIST/dist" | tr -d ' ') cases match source, including enabled protocols)"
