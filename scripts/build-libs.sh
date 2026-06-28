#!/usr/bin/env bash
# build-libs.sh — build pre-compiled CSP library artefacts from dist/ (🎯T24).
#
# Produces, for the host platform, static (.a) and shared (.dylib/.so)
# libraries that downstream users link against WITHOUT any CSP sources or
# vendored deps in their own tree:
#
#   libcsp.{a,dylib|so}            core: csp.cpp + csp_globals.cpp
#   libcsp_<proto>.{a,dylib|so}    one per protocol drop-in (tls, http,
#                                  http2, http3, ws, quic)
#   lib<dep>.{a,dylib|so}          the vendored third-party libraries each
#                                  protocol needs (llhttp, picotls, nghttp2,
#                                  nghttp3, ngtcp2, wslay)
#
# The per-protocol split preserves the dist drop-in's cherry-pick model:
# a downstream app links only `-lcsp -lcsp_http -lllhttp` for HTTP/1.1, and
# linker dead-code elimination still drops anything it doesn't reference.
#
# Build is driven entirely from dist/ so the artefacts match exactly what an
# external user would vendor. The core dist/csp.cpp inlines the fcontext
# context-switching assembly (per-arch asm() blocks), so no separate .S file
# is needed — the core TU is self-contained.
#
# Usage:
#   scripts/build-libs.sh [--out DIR] [--vendor DIR] [--no-fetch] [PROTO...]
#
#   --out DIR      output directory for libraries  (default: build/libs)
#   --vendor DIR   vendored-deps directory          (default: vendor)
#   --no-fetch     don't run vendor-deps.sh; assume --vendor is populated
#   PROTO...       subset of {tls http http2 http3 ws quic}; default: all
#
# Requires: a C++20 / libc++ compiler (CXX), a C compiler (CC), ar, and
# (when fetching) git. Honours CXX / CC / AR from the environment. Works on
# macOS (.dylib) and Linux (.so).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO_ROOT/dist"

OUT_DIR="$REPO_ROOT/build/libs"
# Default to a scratch vendor tree (flat vendor-deps.sh layout), kept out of
# the dev submodule tree under vendor/github.com/. Both are gitignored.
VENDOR_DIR="$REPO_ROOT/build/libs-vendor"
DO_FETCH=1
PROTOS=()

while (( $# )); do
    case "$1" in
        --out)     OUT_DIR="$2"; shift 2 ;;
        --vendor)  VENDOR_DIR="$2"; shift 2 ;;
        --no-fetch) DO_FETCH=0; shift ;;
        -h|--help) sed -n '2,/^set -euo pipefail$/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0 ;;
        tls|http|http2|http3|ws|quic) PROTOS+=("$1"); shift ;;
        *) echo "build-libs: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

(( ${#PROTOS[@]} )) || PROTOS=(tls http http2 http3 ws quic)

CXX_BIN="${CXX:-c++}"
CC_BIN="${CC:-cc}"
AR_BIN="${AR:-ar}"

case "$(uname -s)" in
    Darwin)
        SHLIB_EXT="dylib"
        # ld64: -dead_strip drops unreferenced sections; the per-dylib
        # install_name uses @rpath so a downstream app finds the libs via
        # its own -rpath (or a copied-alongside layout).
        shlib_flags() { printf '%s\n' -dynamiclib -Wl,-dead_strip \
            "-Wl,-install_name,@rpath/$1" -undefined dynamic_lookup; }
        ;;
    Linux)
        SHLIB_EXT="so"
        shlib_flags() { printf '%s\n' -shared -Wl,--gc-sections \
            "-Wl,-soname,$1" -Wl,--allow-shlib-undefined; }
        ;;
    *) echo "build-libs: unsupported OS $(uname -s)" >&2; exit 2 ;;
esac

# Per-protocol → vendor-deps.sh flag and the C-source globs that make up the
# matching third-party library. These mirror scripts/subset_check.sh and the
# project Makefile's source lists, so the artefacts are byte-for-byte the same
# library a source build links.
vendor_flag() {
    case "$1" in
        tls)   echo --tls ;;
        http)  echo --http ;;
        http2) echo --http2 ;;
        http3) echo --http3 ;;
        ws)    echo --ws ;;
        quic)  echo --quic ;;
    esac
}

# Curated picotls minicrypto source list (picotls/lib also has mbedtls.c,
# openssl.c, fusion.c which need backends we don't link).
picotls_srcs() {
    local d="$VENDOR_DIR/picotls"
    printf '%s\n' \
        "$d/lib/picotls.c" "$d/lib/hpke.c" "$d/lib/pembase64.c" \
        "$d/lib/cifra.c" "$d/lib/cifra/x25519.c" "$d/lib/cifra/chacha20.c" \
        "$d/lib/cifra/aes128.c" "$d/lib/cifra/aes256.c" "$d/lib/cifra/random.c" \
        "$d/lib/uecc.c" "$d/lib/minicrypto-pem.c" "$d/lib/asn1.c" "$d/lib/ffx.c" \
        "$d/deps/micro-ecc/uECC.c" \
        "$d/deps/cifra/src/aes.c" "$d/deps/cifra/src/blockwise.c" \
        "$d/deps/cifra/src/chacha20.c" "$d/deps/cifra/src/chash.c" \
        "$d/deps/cifra/src/curve25519.c" "$d/deps/cifra/src/drbg.c" \
        "$d/deps/cifra/src/hmac.c" "$d/deps/cifra/src/gcm.c" \
        "$d/deps/cifra/src/gf128.c" "$d/deps/cifra/src/modes.c" \
        "$d/deps/cifra/src/poly1305.c" "$d/deps/cifra/src/sha256.c" \
        "$d/deps/cifra/src/sha512.c"
}
llhttp_srcs() {
    local d="$VENDOR_DIR/llhttp"
    printf '%s\n' "$d/src/llhttp.c" "$d/src/api.c" "$d/src/http.c"
}
wslay_srcs() {
    local d="$VENDOR_DIR/wslay/lib"
    printf '%s\n' "$d/wslay_frame.c" "$d/wslay_event.c" \
        "$d/wslay_queue.c" "$d/wslay_net.c"
}
nghttp2_srcs() {
    local d="$VENDOR_DIR/nghttp2/lib"
    printf '%s\n' "$d"/nghttp2_*.c "$d/sfparse.c"
}
nghttp3_srcs() {
    local d="$VENDOR_DIR/nghttp3/lib"
    printf '%s\n' "$d"/nghttp3_*.c
}
# ngtcp2 transport + crypto shared + CSP's minicrypto adapter (ships in dist/).
ngtcp2_srcs() {
    local d="$VENDOR_DIR/ngtcp2"
    printf '%s\n' "$d/lib"/ngtcp2_*.c "$d/crypto/shared.c" \
        "$DIST/ngtcp2_crypto_picotls_minicrypto.c"
}

# Include flags and -D defines for the C++ protocol drop-in compile, keyed by
# protocol. The drop-in #includes <csp.h> plus the third-party headers.
INC_TLS="-I$VENDOR_DIR/picotls/include -I$VENDOR_DIR/picotls/deps/cifra/src -I$VENDOR_DIR/picotls/deps/cifra/src/ext -I$VENDOR_DIR/picotls/deps/micro-ecc"
INC_LLHTTP="-I$VENDOR_DIR/llhttp/include"
INC_NGHTTP2="-I$VENDOR_DIR/nghttp2/lib/includes"
INC_NGHTTP3="-I$VENDOR_DIR/nghttp3/lib/includes -I$VENDOR_DIR/nghttp3/lib"
INC_NGTCP2="-I$VENDOR_DIR/ngtcp2/lib/includes -I$VENDOR_DIR/ngtcp2/crypto/includes -I$VENDOR_DIR/ngtcp2/lib -I$VENDOR_DIR/ngtcp2/crypto"
INC_WSLAY="-I$VENDOR_DIR/wslay/lib/includes -I$VENDOR_DIR/wslay/lib"

# C-compiler flags per vendored library (mirror the project Makefile).
cflags_for() {
    case "$1" in
        picotls) echo "$INC_TLS" ;;
        llhttp)  echo "$INC_LLHTTP" ;;
        nghttp2) echo "$INC_NGHTTP2 -DBUILDING_NGHTTP2 -DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1" ;;
        nghttp3) echo "$INC_NGHTTP3 -DBUILDING_NGHTTP3 -DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1" ;;
        ngtcp2)  echo "$INC_NGTCP2 -I$VENDOR_DIR/picotls/include -I$VENDOR_DIR/picotls/deps/cifra/src -I$VENDOR_DIR/picotls/deps/cifra/src/ext -DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1" ;;
        wslay)   echo "$INC_WSLAY -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H" ;;
    esac
}

# Common compile flags: per-section emission so downstream dead-strip works.
CXX_COMMON=(-std=c++20 -stdlib=libc++ -O2 -fPIC
            -ffunction-sections -fdata-sections
            -Wno-unused-function -Wno-deprecated-declarations
            -I"$DIST")
CC_COMMON=(-O2 -fPIC -ffunction-sections -fdata-sections)

WORK="$OUT_DIR/.obj"
mkdir -p "$OUT_DIR" "$WORK"

# --- expand requested protocols to the libraries they need ----------------
# A protocol's drop-in implies its transitive vendor libs and protocol deps
# (matching vendor-deps.sh's transitive resolution). Stock macOS ships bash
# 3.2, which has no associative arrays, so model the two sets as space-padded
# strings (" tls http ") with membership helpers. Iterate with unquoted
# expansion (`for p in $want_proto`) — the tokens are simple identifiers.
want_proto=" "
want_vlib=" "
has()        { case "$1" in *" $2 "*) return 0 ;; *) return 1 ;; esac; }
add_proto()  { has "$want_proto" "$1" || want_proto="$want_proto$1 "; }
add_vlib()   { has "$want_vlib"  "$1" || want_vlib="$want_vlib$1 "; }

for p in "${PROTOS[@]}"; do add_proto "$p"; done
has "$want_proto" http3 && add_proto quic   # chained, order matters:
has "$want_proto" quic  && add_proto tls    # http3 -> quic -> tls
has "$want_proto" http2 && add_proto tls
has "$want_proto" ws    && add_proto http

has "$want_proto" tls   && add_vlib picotls
has "$want_proto" http  && add_vlib llhttp
has "$want_proto" http2 && add_vlib nghttp2
has "$want_proto" http3 && add_vlib nghttp3
has "$want_proto" quic  && add_vlib ngtcp2
has "$want_proto" ws    && add_vlib wslay

# --- fetch vendored deps --------------------------------------------------
if (( DO_FETCH )); then
    flags=()
    for p in $want_proto; do flags+=("$(vendor_flag "$p")"); done
    echo "=== fetching vendored deps: ${flags[*]} ==="
    ( cd "$REPO_ROOT" && VENDOR_DIR="$VENDOR_DIR" "$REPO_ROOT/scripts/vendor-deps.sh" "${flags[@]}" )
fi

# --- helper: archive .o list into static + shared lib ---------------------
# make_lib NAME obj...    →    libNAME.a + libNAME.<ext>
# For the shared lib we leave inter-library symbols undefined (resolved at
# final-app link time), so each lib stays independently shippable.
make_lib() {
    local name="$1"; shift
    local a="$OUT_DIR/lib$name.a"
    local sh="$OUT_DIR/lib$name.$SHLIB_EXT"
    rm -f "$a"
    "$AR_BIN" rcs "$a" "$@"
    # bash 3.2 has no `mapfile`; read the flag lines into an array by hand.
    local flags=() _line
    while IFS= read -r _line; do flags+=("$_line"); done \
        < <(shlib_flags "lib$name.$SHLIB_EXT")
    "$CXX_BIN" -std=c++20 -stdlib=libc++ -fPIC "${flags[@]}" -o "$sh" "$@"
    echo "  -> lib$name.a  lib$name.$SHLIB_EXT"
}

# --- compile a vendored library to .o set, then archive -------------------
build_vlib() {
    local name="$1"; shift
    local -a srcs=("$@")
    local cflags; read -r -a cflags <<< "$(cflags_for "$name")"
    echo "=== vendored lib: $name (${#srcs[@]} sources) ==="
    local objs=() i=0 obj
    for src in "${srcs[@]}"; do
        [[ -f "$src" ]] || { echo "  MISSING: $src" >&2; exit 1; }
        obj="$WORK/${name}_$i.o"
        "$CC_BIN" "${CC_COMMON[@]}" "${cflags[@]}" -c "$src" -o "$obj"
        objs+=("$obj"); i=$((i+1))
    done
    make_lib "$name" "${objs[@]}"
}

# --- compile a CSP protocol drop-in to .o, then archive -------------------
# Each protocol drop-in TU compiles independently (it #includes csp.h which
# declares everything). The matching vendored lib's headers are on the
# include path. csp_quic / csp_http3 need -DCSP_TLS; csp_tls too.
proto_includes() {
    local p="$1" inc=""
    case "$p" in
        tls)   inc="$INC_TLS" ;;
        http)  inc="$INC_LLHTTP" ;;
        http2) inc="$INC_NGHTTP2 $INC_TLS" ;;
        http3) inc="$INC_NGHTTP3 $INC_NGTCP2 $INC_TLS" ;;
        ws)    inc="$INC_WSLAY $INC_LLHTTP" ;;
        quic)  inc="$INC_NGTCP2 $INC_TLS" ;;
    esac
    echo "$inc"
}
proto_defines() {
    case "$1" in
        tls|http2|http3|quic) echo "-DCSP_TLS" ;;
        *) echo "" ;;
    esac
}

build_proto() {
    local p="$1"
    local src="$DIST/csp_$p.cpp"
    [[ -f "$src" ]] || { echo "  MISSING drop-in: $src" >&2; exit 1; }
    local inc def
    read -r -a inc <<< "$(proto_includes "$p")"
    read -r -a def <<< "$(proto_defines "$p")"
    echo "=== protocol lib: csp_$p ==="
    local obj="$WORK/csp_$p.o"
    # def is empty for protocols without -DCSP_TLS (http, ws); guard the
    # expansion so bash 3.2's `set -u` doesn't abort on the empty array.
    "$CXX_BIN" "${CXX_COMMON[@]}" ${def[@]+"${def[@]}"} "${inc[@]}" -c "$src" -o "$obj"
    make_lib "csp_$p" "$obj"
}

# --- core lib (always) ----------------------------------------------------
echo "=== core lib: csp ==="
# Core dist build needs -DCSP_TLS only if a TLS-gated protocol is in the set;
# the core TU itself references no protocol symbols, but csp.h gates a few
# declarations behind CSP_TLS. Building the core with CSP_TLS keeps the ABI
# uniform across the whole artefact set.
CORE_DEF=()
if has "$want_proto" tls || has "$want_proto" quic || \
   has "$want_proto" http2 || has "$want_proto" http3; then
    CORE_DEF+=(-DCSP_TLS)
fi
core_objs=()
for src in csp.cpp csp_globals.cpp; do
    obj="$WORK/${src%.cpp}.o"
    # ${arr[@]+"${arr[@]}"} expands safely when the array is empty — bash 3.2
    # errors on a plain "${empty[@]}" under `set -u` (CORE_DEF is empty for a
    # TLS-free protocol set).
    "$CXX_BIN" "${CXX_COMMON[@]}" ${CORE_DEF[@]+"${CORE_DEF[@]}"} -c "$DIST/$src" -o "$obj"
    core_objs+=("$obj")
done
make_lib csp "${core_objs[@]}"

# --- vendored libs --------------------------------------------------------
for v in $want_vlib; do
    case "$v" in
        picotls) build_vlib picotls $(picotls_srcs) ;;
        llhttp)  build_vlib llhttp  $(llhttp_srcs) ;;
        nghttp2) build_vlib nghttp2 $(nghttp2_srcs) ;;
        nghttp3) build_vlib nghttp3 $(nghttp3_srcs) ;;
        ngtcp2)  build_vlib ngtcp2  $(ngtcp2_srcs) ;;
        wslay)   build_vlib wslay   $(wslay_srcs) ;;
    esac
done

# --- protocol libs --------------------------------------------------------
for p in $want_proto; do
    build_proto "$p"
done

# --- bundle the public header --------------------------------------------
cp "$DIST/csp.h" "$OUT_DIR/csp.h"

echo
echo "=== build-libs: artefacts in $OUT_DIR ==="
ls -1 "$OUT_DIR"/*.a "$OUT_DIR"/*."$SHLIB_EXT" 2>/dev/null | sed 's/^/  /'
