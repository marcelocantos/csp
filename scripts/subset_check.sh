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
#   unreferenced — every drop-in + every library compiled into the link,
#                but the sample references none of them. The only subset
#                whose absent-assertions the linker alone can satisfy: see
#                "What proves DCE" below (🎯T63).
#
# work-dir defaults to a fresh tempdir; CI passes one to keep ccache warm
# across matrix jobs.
#
# What proves DCE (🎯T63). An absent-assertion only tests the linker if the
# library it names was compiled INTO the link. For channels/http/http+ws/
# quic every absent library is simply never compiled, so those assertions
# hold with or without dead-stripping — they test build separation, not
# DCE. Replacing the dead-strip flag with a no-op once passed all five
# original subsets, 29/29. `full` (nghttp3, compiled but unreferenced) and
# `unreferenced` (everything compiled, nothing referenced) are the subsets
# that fail when dead-stripping is removed, and the negative control below
# keeps that true.
#
# Negative control: SUBSET_CHECK_NEGATIVE_CONTROL=1 links WITHOUT dead-
# stripping and inverts the absent-check: it succeeds only if at least one
# asserted-absent library leaks into the binary, and exits EXIT_VACUOUS
# otherwise. A build failure still exits non-zero, so a broken build can
# never be mistaken for a passing control.
#
# Exit codes: 0 ok; EXIT_USAGE (2) bad subset or misclassified library;
# EXIT_SYMBOLS (3) a symbol expectation failed; EXIT_VACUOUS (4) the
# negative control found nothing that depends on the linker.

set -euo pipefail

EXIT_USAGE=2
EXIT_SYMBOLS=3
EXIT_VACUOUS=4

# bash 3.2 compatibility (🎯T62). This script is distributed to external
# users and runs on macOS, whose stock /bin/bash is 3.2.57. Before bash
# 4.4, `"${arr[@]}"` on an *empty* array is an unbound-variable error
# under `set -u`, which aborts the run. Every array below that can be
# empty for some subset — DROPIN_PROTO, DEFINES, INCLUDES, C_SRCS,
# c_objs, EXPECTED_PRESENT (channels, unreferenced), LINK_DCE_FLAGS
# (negative control) — is
# therefore expanded as `${arr[@]+"${arr[@]}"}`, which yields nothing
# when the array is unset/empty and the normally-quoted elements
# otherwise. Do not "simplify" these back to plain `"${arr[@]}"`.

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
    *)      echo "subset_check: unsupported OS $(uname -s)" >&2; exit "$EXIT_USAGE" ;;
esac

# An array, not a string: an empty "$DEAD_STRIP_FLAG" passed to the
# compiler driver is an empty-filename argument, not "no flag".
NEGATIVE_CONTROL="${SUBSET_CHECK_NEGATIVE_CONTROL:-0}"
LINK_DCE_FLAGS=("$DEAD_STRIP_FLAG")
if [[ "$NEGATIVE_CONTROL" == 1 ]]; then
    LINK_DCE_FLAGS=()
fi

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
    full|unreferenced)
        DROPIN_PROTO+=(csp_tls.cpp csp_http.cpp csp_http2.cpp csp_ws.cpp csp_quic.cpp csp_http3.cpp)
        VENDOR_FLAGS+=(--all)
        DEFINES+=(-DCSP_TLS
                  -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H
                  -DBUILDING_NGHTTP2 -DBUILDING_NGHTTP3)
        if [[ "$SUBSET" == full ]]; then
            DEFINES+=(-DSUBSET_HTTP -DSUBSET_HTTP2 -DSUBSET_HTTP3
                      -DSUBSET_WS -DSUBSET_QUIC)
        fi
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
        if [[ "$SUBSET" == full ]]; then
            # SUBSET_HTTP3 takes &csp::http3::serve, but http3::serve is a
            # stub (T3.9, blocked on T3.8 QUIC transport) that references
            # no nghttp3 symbol. nghttp3 is compiled into this link and
            # dead-stripping must remove it — a real DCE assertion. When
            # T3.9 lands, move nghttp3 to EXPECTED_PRESENT.
            EXPECTED_PRESENT=("$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS")
            EXPECTED_ABSENT=("$SYM_NGHTTP3")
        else
            # Everything is compiled in and nothing is referenced, so every
            # library must be stripped. This is DCE rules 3 and 5 of
            # docs/design/per-protocol-dist.md made executable: static
            # registration in a protocol TU, or a protocol reference from
            # the front door, would keep a library alive here.
            EXPECTED_ABSENT=("$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS")
        fi
        ;;
    *)
        echo "subset_check: unknown subset '$SUBSET'" >&2
        echo "  expected one of: channels, http, http+ws, quic, full, unreferenced" >&2
        exit "$EXIT_USAGE"
        ;;
esac

# Every library must be classified exactly once per subset — present or
# absent (🎯T63). An unclassified library is asserted by nothing, which is
# how `full` once left nghttp3's dead-stripping unchecked.
for lib in "$SYM_LLHTTP" "$SYM_NGHTTP2" "$SYM_NGHTTP3" "$SYM_NGTCP2" "$SYM_WSLAY" "$SYM_PICOTLS"; do
    n=0
    for p in ${EXPECTED_PRESENT[@]+"${EXPECTED_PRESENT[@]}"} ${EXPECTED_ABSENT[@]+"${EXPECTED_ABSENT[@]}"}; do
        if [[ "$p" == "$lib" ]]; then
            n=$((n+1))
        fi
    done
    if (( n != 1 )); then
        echo "subset_check: subset=$SUBSET classifies $lib $n time(s); each library must be exactly one of present/absent" >&2
        exit "$EXIT_USAGE"
    fi
done

echo "=== subset_check: subset=$SUBSET, work_dir=$WORK_DIR ==="
if [[ "$NEGATIVE_CONTROL" == 1 ]]; then
    echo "=== NEGATIVE CONTROL: linking WITHOUT dead-stripping; an absent library must leak ==="
fi

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
    full|unreferenced)
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
           ${DEFINES[@]+"${DEFINES[@]}"}
           ${INCLUDES[@]+"${INCLUDES[@]}"})
cc_flags=(-O2 -ffunction-sections -fdata-sections
          ${DEFINES[@]+"${DEFINES[@]}"}
          ${INCLUDES[@]+"${INCLUDES[@]}"})

cpp_objs=()
for src in "${DROPIN_CORE[@]}" ${DROPIN_PROTO[@]+"${DROPIN_PROTO[@]}"}; do
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
for src in ${C_SRCS[@]+"${C_SRCS[@]}"}; do
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
"$CXX_BIN" -std=c++20 -stdlib=libc++ ${LINK_DCE_FLAGS[@]+"${LINK_DCE_FLAGS[@]}"} \
    sample.o "${cpp_objs[@]}" ${c_objs[@]+"${c_objs[@]}"} -o sample

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
leaked=0
for prefix in ${EXPECTED_PRESENT[@]+"${EXPECTED_PRESENT[@]}"}; do
    if grep -qE "$prefix" <<< "$SYMS"; then
        echo "  ✓ present: $prefix"
    else
        echo "  ✗ MISSING (should be present): $prefix" >&2
        fail=1
    fi
done
for prefix in ${EXPECTED_ABSENT[@]+"${EXPECTED_ABSENT[@]}"}; do
    if grep -qE "$prefix" <<< "$SYMS"; then
        leaked=$((leaked+1))
        if [[ "$NEGATIVE_CONTROL" == 1 ]]; then
            echo "  ✓ leaked without dead-strip: $prefix"
        else
            # `grep -m`, not `grep | head`: head closing the pipe early
            # SIGPIPEs grep, pipefail fails the assignment, and set -e then
            # killed the script (exit 141) before this report printed. It
            # went unnoticed because no leak had ever reached this branch.
            offenders=$(grep -m 5 -E "$prefix" <<< "$SYMS")
            echo "  ✗ LEAKED (should be absent): $prefix" >&2
            sed 's/^/      /' <<< "$offenders" >&2
            fail=1
        fi
    else
        echo "  ✓ absent:  $prefix"
    fi
done

if (( fail )); then
    echo
    echo "subset_check FAILED for subset=$SUBSET" >&2
    exit "$EXIT_SYMBOLS"
fi

if [[ "$NEGATIVE_CONTROL" == 1 ]]; then
    echo
    if (( leaked == 0 )); then
        echo "subset_check: NEGATIVE CONTROL VACUOUS for subset=$SUBSET — no absent library leaked" >&2
        echo "  without dead-stripping, so these assertions do not depend on the linker." >&2
        exit "$EXIT_VACUOUS"
    fi
    echo "subset_check: negative control for subset=$SUBSET — $leaked absent library prefix(es) leak without dead-stripping, so the absent-assertions depend on the linker."
    exit 0
fi

echo
echo "subset_check: subset=$SUBSET — all symbol expectations met."
