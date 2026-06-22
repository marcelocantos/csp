#!/usr/bin/env bash
# vendor-deps.sh — fetch third-party libraries needed by the CSP dist drop-in.
#
# CSP ships as a small set of source files in dist/ that users vendor into
# their own project. The per-protocol .cpp files depend on third-party C99
# libraries (llhttp, picotls, nghttp2, etc.). This script fetches known-good
# versions of those libraries into vendor/ with the layout the drop-in's
# documented compile recipes (see dist/AGENTS-CSP.md "Integration") expect.
#
# Usage:
#   ./scripts/vendor-deps.sh --all                 fetch every library
#   ./scripts/vendor-deps.sh --http --ws           HTTP/1.1 + WebSocket
#   ./scripts/vendor-deps.sh --http3               HTTP/3 (pulls QUIC + TLS)
#   ./scripts/vendor-deps.sh --help                show this help
#
# Flags map to drop-in protocol .cpp files:
#
#   Flag       Drop-in       Libraries fetched (transitive)
#   --------   -----------   -----------------------------------------------
#   --tls      csp_tls.cpp   picotls
#   --http     csp_http.cpp  llhttp
#   --http2    csp_http2.cpp nghttp2 (+ picotls for serve_tls / ALPN)
#   --ws       csp_ws.cpp    wslay (+ llhttp for the upgrade handshake)
#   --quic     csp_quic.cpp  ngtcp2, picotls
#   --http3    csp_http3.cpp nghttp3, ngtcp2, picotls
#   --all      everything    every library above
#
# Output layout (matches dist/AGENTS-CSP.md include paths):
#
#   vendor/llhttp/      — clone of nodejs/llhttp
#   vendor/picotls/     — clone of h2o/picotls (cifra + micro-ecc included)
#   vendor/nghttp2/     — clone of nghttp2/nghttp2 (with nghttp2ver.h generated)
#   vendor/nghttp3/     — clone of ngtcp2/nghttp3   (with version.h generated)
#   vendor/ngtcp2/      — clone of ngtcp2/ngtcp2    (with version.h generated)
#   vendor/wslay/       — clone of tatsuhiro-t/wslay (with wslayver.h stub)
#
# Versions are pinned to commits that the CSP project's own CI builds against.
# Re-running the script is idempotent — already-fetched libraries at the
# pinned commit are skipped.
#
# Requires: git, sed. Works on macOS and Linux.

set -euo pipefail

# ---------------------------------------------------------------------------
# Pinned versions. These commits match the CSP repo's own submodule pins as
# of the script's last update. Bumping a pin requires re-testing the dist
# build against the new revision.

PIN_LLHTTP_TAG='release/v9.3.1'                                # nodejs/llhttp
PIN_PICOTLS='bb37a1709889eba08c6bfcdf5ef477e3d7008302'         # h2o/picotls
PIN_NGHTTP2='7d8e587ad4e8da4b772725bd345d48ec0d42b2ec'         # v1.69.0-40
PIN_NGHTTP3='e7a5a617ddf04b46eb7ce0ddaf3e318b1b46017d'         # v1.15.0-27
PIN_NGTCP2='88f9f6576226ea7fd4aeb81ce809a71a3ae03413'          # v1.22.0-85
PIN_WSLAY_TAG='release-1.1.1'                                  # tatsuhiro-t/wslay

NGHTTP2_VERSION_STR='1.69.90'
NGHTTP2_VERSION_NUM='0x01455a'
NGHTTP3_VERSION_STR='1.15.0'
NGHTTP3_VERSION_NUM='0x010f00'
NGTCP2_VERSION_STR='1.22.0'
NGTCP2_VERSION_NUM='0x011600'

# Repo root = parent of scripts/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Destination for the fetched libraries. Defaults to vendor/ alongside the
# script; override with VENDOR_DIR to fetch into a scratch tree (the
# pre-built-lib build uses this so it doesn't disturb the dev submodule tree).
VENDOR_DIR="${VENDOR_DIR:-$REPO_ROOT/vendor}"

# Selected protocols (set by argument parsing).
WANT_TLS=0
WANT_HTTP=0
WANT_HTTP2=0
WANT_HTTP3=0
WANT_WS=0
WANT_QUIC=0

# ---------------------------------------------------------------------------

log() { printf '[vendor-deps] %s\n' "$*" >&2; }
die() { printf '[vendor-deps] error: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,/^set -euo pipefail$/p' "$0" | sed 's/^# \{0,1\}//;$d' >&2
    exit "${1:-0}"
}

while (( $# )); do
    case "$1" in
        --tls)     WANT_TLS=1 ;;
        --http)    WANT_HTTP=1 ;;
        --http2)   WANT_HTTP2=1 ;;
        --http3)   WANT_HTTP3=1 ;;
        --ws)      WANT_WS=1 ;;
        --quic)    WANT_QUIC=1 ;;
        --all)
            WANT_TLS=1; WANT_HTTP=1; WANT_HTTP2=1; WANT_HTTP3=1
            WANT_WS=1;  WANT_QUIC=1
            ;;
        -h|--help) usage 0 ;;
        *)         log "unknown argument: $1"; usage 1 ;;
    esac
    shift
done

if (( WANT_TLS + WANT_HTTP + WANT_HTTP2 + WANT_HTTP3 + WANT_WS + WANT_QUIC == 0 )); then
    log 'no protocol flags given; pass --all or one of --tls --http --http2 --http3 --ws --quic'
    usage 1
fi

# Resolve transitive dependencies.
(( WANT_HTTP3 )) && { WANT_QUIC=1; }                       # http3 → quic + nghttp3
(( WANT_QUIC ))  && { WANT_TLS=1; }                        # quic  → tls
(( WANT_HTTP2 )) && { WANT_TLS=1; }                        # http2 → tls (for serve_tls)
(( WANT_WS ))    && { WANT_HTTP=1; }                       # ws    → http (for upgrade)

command -v git >/dev/null || die 'git not found in PATH'
command -v sed >/dev/null || die 'sed not found in PATH'

mkdir -p "$VENDOR_DIR"

# ---------------------------------------------------------------------------
# fetch_repo <local-dir> <upstream-url> <ref>
#
# Clones <upstream-url> to vendor/<local-dir> and checks out <ref>. If the
# directory already exists and is at the requested ref, skips. Otherwise
# fetches and updates.

fetch_repo() {
    local name="$1" url="$2" ref="$3"
    local dest="$VENDOR_DIR/$name"

    if [[ -d "$dest/.git" ]]; then
        local head
        head="$(git -C "$dest" rev-parse HEAD)"
        if [[ "$head" == "$ref" ]] || git -C "$dest" describe --tags --exact-match "$head" 2>/dev/null | grep -qx "$ref"; then
            log "$name already at $ref — skipping"
            return
        fi
        log "$name updating to $ref"
        git -C "$dest" fetch --tags --depth=1 origin "$ref" 2>/dev/null || git -C "$dest" fetch --tags origin
    else
        log "$name cloning $url"
        git clone --filter=blob:none "$url" "$dest"
    fi

    git -C "$dest" checkout --quiet "$ref"
    git -C "$dest" submodule update --init --recursive --quiet
}

# version_h <path> <PACKAGE_VERSION> <PACKAGE_VERSION_NUM>
#
# Generates a version.h from version.h.in by substituting the @PACKAGE_VERSION@
# and @PACKAGE_VERSION_NUM@ placeholders. nghttp2/nghttp3/ngtcp2 all use this
# scheme; their build systems would normally produce these via autotools.

version_h() {
    local in="$1.in" out="$1" ver_str="$2" ver_num="$3"
    [[ -f "$in" ]] || die "missing $in"
    sed -e "s/@PACKAGE_VERSION@/$ver_str/" \
        -e "s/@PACKAGE_VERSION_NUM@/$ver_num/" \
        "$in" > "$out"
    log "$(basename "$out") generated"
}

# ---------------------------------------------------------------------------
# Per-library fetchers.

fetch_llhttp() {
    fetch_repo llhttp https://github.com/nodejs/llhttp.git "$PIN_LLHTTP_TAG"
}

fetch_picotls() {
    fetch_repo picotls https://github.com/h2o/picotls.git "$PIN_PICOTLS"
    # cifra and micro-ecc are checked into picotls as subtrees (not submodules),
    # so the recursive update above is sufficient.
    [[ -d "$VENDOR_DIR/picotls/deps/cifra" ]]     || die 'picotls/deps/cifra missing after clone'
    [[ -d "$VENDOR_DIR/picotls/deps/micro-ecc" ]] || die 'picotls/deps/micro-ecc missing after clone'
}

fetch_nghttp2() {
    fetch_repo nghttp2 https://github.com/nghttp2/nghttp2.git "$PIN_NGHTTP2"
    version_h \
        "$VENDOR_DIR/nghttp2/lib/includes/nghttp2/nghttp2ver.h" \
        "$NGHTTP2_VERSION_STR" "$NGHTTP2_VERSION_NUM"
}

fetch_nghttp3() {
    fetch_repo nghttp3 https://github.com/ngtcp2/nghttp3.git "$PIN_NGHTTP3"
    version_h \
        "$VENDOR_DIR/nghttp3/lib/includes/nghttp3/version.h" \
        "$NGHTTP3_VERSION_STR" "$NGHTTP3_VERSION_NUM"
}

fetch_ngtcp2() {
    fetch_repo ngtcp2 https://github.com/ngtcp2/ngtcp2.git "$PIN_NGTCP2"
    version_h \
        "$VENDOR_DIR/ngtcp2/lib/includes/ngtcp2/version.h" \
        "$NGTCP2_VERSION_STR" "$NGTCP2_VERSION_NUM"
}

fetch_wslay() {
    fetch_repo wslay https://github.com/tatsuhiro-t/wslay.git "$PIN_WSLAY_TAG"
    # wslay's wslayver.h is normally generated by autotools. We ship a static
    # stub that matches the latest tagged release; the drop-in only reads
    # the version macros for diagnostics, never for ABI selection.
    cat > "$VENDOR_DIR/wslay/lib/includes/wslay/wslayver.h" <<'EOF'
/* wslay version header — generated stub for the dist drop-in build. */
#ifndef WSLAY_VERSION_H
#define WSLAY_VERSION_H

#define WSLAY_VERSION       "1.1.1"
#define WSLAY_VERSION_MAJOR 1
#define WSLAY_VERSION_MINOR 1
#define WSLAY_VERSION_PATCH 1
#define WSLAY_VERSION_NUM   0x010101

#endif /* WSLAY_VERSION_H */
EOF
    log 'wslayver.h stub written'
}

# ---------------------------------------------------------------------------
# Dispatch.

(( WANT_TLS ))   && fetch_picotls
(( WANT_HTTP ))  && fetch_llhttp
(( WANT_HTTP2 )) && fetch_nghttp2
(( WANT_HTTP3 )) && fetch_nghttp3
(( WANT_QUIC ))  && fetch_ngtcp2
(( WANT_WS ))    && fetch_wslay

log 'done.'
