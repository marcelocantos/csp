#!/usr/bin/env bash
# subset_check.sh — verify the per-protocol drop-in's linker dead-code
# elimination works as advertised. Fetches the third-party libraries for
# a subset configuration, compiles dist + the subset sample, links, and
# uses `nm` to confirm that libraries belonging to unselected protocols
# do NOT contribute any symbols to the final binary. Implements 🎯T23.3.
#
# Usage: scripts/subset_check.sh <subset> [<work-dir>]
#
# Subsets:
#   channels   — csp.cpp + csp_globals.cpp only; no third-party libs.
#   http       — + csp_http.cpp + llhttp.
#   http+ws    — + csp_ws.cpp + wslay (transitively pulls http via the
#                upgrade flow).
#   quic       — + csp_quic.cpp + csp_tls.cpp + ngtcp2 + picotls.
#   full       — every drop-in + every library.
#
# work-dir defaults to a fresh tempdir; CI passes one to keep ccache warm
# across matrix jobs.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SUBSET="${1:-}"
WORK_DIR="${2:-$(mktemp -d -t subset-check.XXXXXX)}"

# Compiler overrides. CI's Linux matrix uses clang-18 for libc++ support;
# macOS uses the system clang. Source builds use `cc`/`c++` by default.
CXX_BIN="${CXX:-c++}"
CC_BIN="${CC:-cc}"

case "$(uname -s)" in
    Darwin) DEAD_STRIP_FLAG="-Wl,-dead_strip" ;;
    Linux)  DEAD_STRIP_FLAG="-Wl,--gc-sections" ;;
    *)      echo "subset_check: unsupported OS $(uname -s)" >&2; exit 2 ;;
esac

# Symbol prefixes by library. Each matches an optional leading underscore
# so the same regex works on macOS (Mach-O prefixes C symbols with `_`)
# and Linux (ELF doesn't).
SYM_LLHTTP='^_?llhttp_'
SYM_NGHTTP2='^_?nghttp2_'
SYM_NGHTTP3='^_?nghttp3_'
SYM_NGTCP2='^_?ngtcp2_'
SYM_WSLAY='^_?wslay_'
SYM_PICOTLS='^_?ptls_'

# Curated picotls minicrypto source list — mirrors CSP's project Makefile
# PICOTLS_SRCS. picotls/lib/ also contains mbedtls.c, openssl.c, fusion.c,
# etc., which require backends we don't link — globbing breaks the build.
picotls_srcs() {
    local d="$1"
    printf '%s\n' \
        "$d/picotls/lib/picotls.c" \
        "$d/picotls/lib/hpke.c" \
        "$d/picotls/lib/pembase64.c" \
        "$d/picotls/lib/cifra.c" \
        "$d/picotls/lib/cifra/x25519.c" \
        "$d/picotls/lib/cifra/chacha20.c" \
        "$d/picotls/lib/cifra/aes128.c" \
        "$d/picotls/lib/cifra/aes256.c" \
        "$d/picotls/lib/cifra/random.c" \
        "$d/picotls/lib/uecc.c" \
        "$d/picotls/lib/minicrypto-pem.c" \
        "$d/picotls/lib/asn1.c" \
        "$d/picotls/lib/ffx.c" \
        "$d/picotls/deps/micro-ecc/uECC.c" \
        "$d/picotls/deps/cifra/src/aes.c" \
        "$d/picotls/deps/cifra/src/blockwise.c" \
        "$d/picotls/deps/cifra/src/chacha20.c" \
        "$d/picotls/deps/cifra/src/chash.c" \
        "$d/picotls/deps/cifra/src/curve25519.c" \
        "$d/picotls/deps/cifra/src/drbg.c" \
        "$d/picotls/deps/cifra/src/hmac.c" \
        "$d/picotls/deps/cifra/src/gcm.c" \
        "$d/picotls/deps/cifra/src/gf128.c" \
        "$d/picotls/deps/cifra/src/modes.c" \
        "$d/picotls/deps/cifra/src/poly1305.c" \
        "$d/picotls/deps/cifra/src/sha256.c" \
        "$d/picotls/deps/cifra/src/sha512.c"
}

llhttp_srcs() {
    local d="$1"
    printf '%s\n' \
        "$d/llhttp/src/llhttp.c" \
        "$d/llhttp/src/api.c" \
        "$d/llhttp/src/http.c"
}

wslay_srcs() {
    local d="$1"
    printf '%s\n' \
        "$d/wslay/lib/wslay_frame.c" \
        "$d/wslay/lib/wslay_event.c" \
        "$d/wslay/lib/wslay_queue.c" \
        "$d/wslay/lib/wslay_net.c"
}

# --- per-subset configuration ----------------------------------------

DROPIN_CORE=(csp.cpp csp_globals.cpp)
DROPIN_PROTO=()
VENDOR_FLAGS=()
DEFINES=()
INCLUDES=()
EXPECTED_PRESENT=()
EXPECTED_ABSENT=()

case "$SUBSET" in
    channels)
        EXPECTED_ABSENT=("$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS")
        ;;
    http)
        DROPIN_PROTO+=(csp_http.cpp)
        VENDOR_FLAGS+=(--http)
        DEFINES+=(-DSUBSET_HTTP)
        INCLUDES+=(-I "$WORK_DIR/vendor/llhttp/include")
        EXPECTED_PRESENT=("$SYM_LLHTTP")
        EXPECTED_ABSENT=("$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS")
        ;;
    http+ws)
        DROPIN_PROTO+=(csp_http.cpp csp_ws.cpp)
        VENDOR_FLAGS+=(--http --ws)
        DEFINES+=(-DSUBSET_HTTP -DSUBSET_WS -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H)
        INCLUDES+=(
            -I "$WORK_DIR/vendor/llhttp/include"
            -I "$WORK_DIR/vendor/wslay/lib/includes"
            -I "$WORK_DIR/vendor/wslay/lib"
        )
        EXPECTED_PRESENT=("$SYM_LLHTTP" "$SYM_WSLAY")
        EXPECTED_ABSENT=("$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_NGTCP2" "$SYM_PICOTLS")
        ;;
    quic)
        DROPIN_PROTO+=(csp_tls.cpp csp_quic.cpp)
        VENDOR_FLAGS+=(--quic)
        DEFINES+=(-DCSP_TLS -DSUBSET_QUIC)
        INCLUDES+=(
            -I "$WORK_DIR/vendor/picotls/include"
            -I "$WORK_DIR/vendor/picotls/deps/cifra/src"
            -I "$WORK_DIR/vendor/picotls/deps/cifra/src/ext"
            -I "$WORK_DIR/vendor/picotls/deps/micro-ecc"
            -I "$WORK_DIR/vendor/ngtcp2/lib/includes"
            -I "$WORK_DIR/vendor/ngtcp2/crypto/includes"
            -I "$WORK_DIR/vendor/ngtcp2/lib"
            -I "$WORK_DIR/vendor/ngtcp2/crypto"
        )
        EXPECTED_PRESENT=("$SYM_NGTCP2" "$SYM_PICOTLS")
        EXPECTED_ABSENT=("$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_WSLAY")
        ;;
    full)
        DROPIN_PROTO+=(csp_tls.cpp csp_http.cpp csp_http2.cpp csp_ws.cpp csp_quic.cpp csp_http3.cpp)
        VENDOR_FLAGS+=(--all)
        # NB: SUBSET_HTTP3 *would* keep nghttp3 alive via &csp::http3::serve,
        # except http3::serve is currently a stub (T3.9, blocked on T3.8
        # QUIC transport). nghttp3 still gets compiled into the build, but
        # dead-strip removes it — correctly — because nothing live references
        # nghttp3 symbols. When T3.9 lands, the SUBSET_HTTP3 reference plus
        # nghttp3 to EXPECTED_PRESENT below.
        DEFINES+=(-DCSP_TLS -DSUBSET_HTTP -DSUBSET_HTTP2 -DSUBSET_HTTP3
                  -DSUBSET_WS -DSUBSET_QUIC
                  -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H
                  -DBUILDING_NGHTTP2 -DBUILDING_NGHTTP3)
        INCLUDES+=(
            -I "$WORK_DIR/vendor/llhttp/include"
            -I "$WORK_DIR/vendor/picotls/include"
            -I "$WORK_DIR/vendor/picotls/deps/cifra/src"
            -I "$WORK_DIR/vendor/picotls/deps/cifra/src/ext"
            -I "$WORK_DIR/vendor/picotls/deps/micro-ecc"
            -I "$WORK_DIR/vendor/nghttp2/lib/includes"
            -I "$WORK_DIR/vendor/nghttp3/lib/includes"
            -I "$WORK_DIR/vendor/nghttp3/lib"
            -I "$WORK_DIR/vendor/ngtcp2/lib/includes"
            -I "$WORK_DIR/vendor/ngtcp2/crypto/includes"
            -I "$WORK_DIR/vendor/ngtcp2/lib"
            -I "$WORK_DIR/vendor/ngtcp2/crypto"
            -I "$WORK_DIR/vendor/wslay/lib/includes"
            -I "$WORK_DIR/vendor/wslay/lib"
        )
        EXPECTED_PRESENT=("$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS")
        ;;
    *)
        echo "subset_check: unknown subset '$SUBSET'" >&2
        echo "  expected one of: channels, http, http+ws, quic, full" >&2
        exit 2
        ;;
esac

echo "=== subset_check: subset=$SUBSET, work_dir=$WORK_DIR ==="

# --- stage the dist drop-in into the work dir -----------------------

mkdir -p "$WORK_DIR/dist" "$WORK_DIR/scripts" "$WORK_DIR/test"
cp -R "$REPO_ROOT/dist/." "$WORK_DIR/dist/"
cp "$REPO_ROOT/scripts/vendor-deps.sh" "$WORK_DIR/scripts/"
cp "$REPO_ROOT/test/dist_subset_sample.cc" "$WORK_DIR/test/"

# --- fetch vendor libs ----------------------------------------------

if (( ${#VENDOR_FLAGS[@]} > 0 )); then
    ( cd "$WORK_DIR" && ./scripts/vendor-deps.sh "${VENDOR_FLAGS[@]}" )
fi

# --- assemble third-party .c source list (after fetch) --------------

C_SRCS=()
add_srcs() {
    local f
    while IFS= read -r f; do
        if [[ -f "$f" ]]; then
            C_SRCS+=("$f")
        else
            echo "  WARN: expected source missing: $f" >&2
        fi
    done
}

case "$SUBSET" in
    http)
        add_srcs < <(llhttp_srcs "$WORK_DIR/vendor")
        ;;
    http+ws)
        add_srcs < <(llhttp_srcs "$WORK_DIR/vendor")
        add_srcs < <(wslay_srcs "$WORK_DIR/vendor")
        ;;
    quic)
        add_srcs < <(picotls_srcs "$WORK_DIR/vendor")
        for f in "$WORK_DIR/vendor/ngtcp2/lib"/ngtcp2_*.c "$WORK_DIR/vendor/ngtcp2/crypto/shared.c"; do
            C_SRCS+=("$f")
        done
        C_SRCS+=("$WORK_DIR/dist/ngtcp2_crypto_picotls_minicrypto.c")
        ;;
    full)
        add_srcs < <(llhttp_srcs "$WORK_DIR/vendor")
        add_srcs < <(picotls_srcs "$WORK_DIR/vendor")
        add_srcs < <(wslay_srcs "$WORK_DIR/vendor")
        for f in "$WORK_DIR/vendor/nghttp2/lib"/nghttp2_*.c "$WORK_DIR/vendor/nghttp2/lib/sfparse.c"; do
            C_SRCS+=("$f")
        done
        for f in "$WORK_DIR/vendor/nghttp3/lib"/nghttp3_*.c; do
            C_SRCS+=("$f")
        done
        for f in "$WORK_DIR/vendor/ngtcp2/lib"/ngtcp2_*.c "$WORK_DIR/vendor/ngtcp2/crypto/shared.c"; do
            C_SRCS+=("$f")
        done
        C_SRCS+=("$WORK_DIR/dist/ngtcp2_crypto_picotls_minicrypto.c")
        ;;
esac

# --- compile + link the sample --------------------------------------

cd "$WORK_DIR"

cxx_flags=(-std=c++20 -stdlib=libc++ -O2
           -ffunction-sections -fdata-sections
           -Wno-unused-function -Wno-deprecated-declarations
           -I dist
           "${DEFINES[@]}"
           "${INCLUDES[@]}")
cc_flags=(-O2 -ffunction-sections -fdata-sections
          "${INCLUDES[@]}")

cpp_objs=()
for src in "${DROPIN_CORE[@]}" "${DROPIN_PROTO[@]}"; do
    out="${src%.cpp}.o"
    echo "  cxx $src"
    "$CXX_BIN" "${cxx_flags[@]}" -c "dist/$src" -o "$out"
    cpp_objs+=("$out")
done

echo "  cxx test/dist_subset_sample.cc"
"$CXX_BIN" "${cxx_flags[@]}" -c test/dist_subset_sample.cc -o sample.o

echo "  (${#C_SRCS[@]} vendored .c files to compile)"
c_objs=()
i=0
for src in "${C_SRCS[@]}"; do
    out="vlib_${i}.o"
    if ! "$CC_BIN" "${cc_flags[@]}" -c "$src" -o "$out" 2> "vlib_${i}.log"; then
        echo "  FAIL compiling $src" >&2
        sed -n '1,20p' "vlib_${i}.log" >&2
        exit 1
    fi
    c_objs+=("$out")
    i=$((i+1))
done

echo "  link"
"$CXX_BIN" -std=c++20 -stdlib=libc++ "$DEAD_STRIP_FLAG" \
    sample.o "${cpp_objs[@]}" "${c_objs[@]}" -o sample

# --- verify symbol presence/absence --------------------------------

echo
echo "=== verifying symbols ==="

# `nm -P | awk ...` extracts defined-symbol names. We pipe through awk
# (not into a here-string at this step) because nm output is unbounded;
# the result is bounded so storing it in a variable is fine for the
# verification loop below.
SYMS=$(nm -P sample 2>/dev/null | awk '$2 != "U" { print $1 }')

# Use here-strings (`grep ... <<< "$SYMS"`) for the per-prefix checks
# below — piping `echo "$SYMS" | grep -q` causes early-exit grep to
# SIGPIPE the upstream `echo`, which `pipefail` then propagates as a
# pipeline failure, making the if-condition spuriously NO-match.

fail=0
for prefix in "${EXPECTED_PRESENT[@]}"; do
    if grep -qE "$prefix" <<< "$SYMS"; then
        echo "  ✓ present: $prefix"
    else
        echo "  ✗ MISSING (should be present): $prefix" >&2
        fail=1
    fi
done
for prefix in "${EXPECTED_ABSENT[@]}"; do
    if grep -qE "$prefix" <<< "$SYMS"; then
        offenders=$(grep -E "$prefix" <<< "$SYMS" | head -5)
        echo "  ✗ LEAKED (should be absent): $prefix" >&2
        sed 's/^/      /' <<< "$offenders" >&2
        fail=1
    else
        echo "  ✓ absent:  $prefix"
    fi
done

if (( fail )); then
    echo
    echo "subset_check FAILED for subset=$SUBSET" >&2
    exit 1
fi

echo
echo "subset_check: subset=$SUBSET — all symbol expectations met."
