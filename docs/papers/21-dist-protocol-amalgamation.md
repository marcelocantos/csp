# Paper 21: Distribution Amalgamation for Protocol Implementations

**Status**: Design proposal. No implementation until approach is approved.

**Related targets**: 🎯T22

---

## Problem Statement

CSP's dist bundle (`csp.h` + `csp.cpp` + `csp_globals.cpp`) excludes five
protocol source files — `http.cc`, `http2.cc`, `http3.cc`, `ws.cc`,
`quic.cc` — because each depends on a vendored C library whose headers and
source cannot be trivially inlined into `csp.cpp`. The consequence is that
dist users cannot use any CSP protocol layer without separately cloning the
vendor submodules and setting up include paths. 🎯T22 asks us to fix this.

Before any implementation, this paper inventories the moving parts and
decides between the two candidate designs.

---

## Actors and Components

1. **`scripts/amalgamate.py`** — reads `src/*.cc`/`src/*.cpp`, strips
   `csp/` includes, and writes `dist/csp.cpp`. Currently excludes all five
   protocol files via an explicit `excluded_sources` set.

2. **Five protocol `.cc` files** — thin C++ wrappers that bridge CSP channels
   to vendored C libraries:

   | File | CSP namespace | Vendored dep(s) | Lines |
   |---|---|---|---|
   | `src/http.cc` | `csp::http` | llhttp | 787 |
   | `src/http2.cc` | `csp::http2` | nghttp2 | 736 |
   | `src/ws.cc` | `csp::ws` | wslay | 752 |
   | `src/http3.cc` | `csp::http3` | nghttp3 | 125 (stub) |
   | `src/quic.cc` | `csp::quic` | ngtcp2 + PicoTLS | 1,502 |
   | **Total** | | | **3,902** |

3. **Vendored C libraries** (submodules, C99) — the source that must also
   be compiled when using any of the above:

   | Library | Used by | Lines (used sources only) | Version header? |
   |---|---|---|---|
   | llhttp | http.cc | 11,688 | No (pre-generated) |
   | nghttp2 | http2.cc | 23,575 | `nghttp2ver.h` from `.in` |
   | wslay | ws.cc | 1,993 | No |
   | nghttp3 | http3.cc | 17,966 | `version.h` from `.in` |
   | ngtcp2 | quic.cc | ~33,500 (lib + crypto/picotls + adapter) | `version.h` from `.in` |

4. **Generated version headers** — three of the five deps generate a
   `version.h` from a `.h.in` template at build time. The Makefile already
   substitutes version constants with `sed`, producing stable checked-in
   files for ngtcp2 (`version.h` is already present in the working tree at
   `vendor/github.com/ngtcp2/ngtcp2/lib/includes/ngtcp2/version.h`).
   nghttp2 and nghttp3 use the same pattern. All three are pinned to exact
   versions, so the generated headers are stable and can be committed.

5. **`make test-dist`** — currently runs the full test suite with
   `CSP_INCLUDE=dist`, but already suppresses protocol tests
   (`test/net.test.cc`, `test/http.test.cc`, …, `test/quic.test.cc`) because
   their headers are absent from `dist/`. For 🎯T22 to pass, the QUIC tests
   at minimum must be exercised by `make test-dist`.

6. **`CSP_TLS` macro** — TLS (PicoTLS) is compiled unconditionally unless
   `CSP_TLS=0`. QUIC depends on TLS, so QUIC support in dist is conditional
   on `CSP_TLS=1`.

7. **`ngtcp2_crypto_picotls_minicrypto.c`** — a C source file in `src/`.
   It is a C99 file, not C++. Any amalgamation that includes it must either
   compile it separately or wrap it with `extern "C"`.

---

## Cross-Reference Map

The five protocol files are self-contained relative to each other:

- `http3.cc` references `quic::connection` conceptually (stub, not yet
  implemented; the stubs just throw) but does not `#include` `quic.h`.
- No other cross-references exist between the five files.
- All five include their own CSP header (`csp/http.h`, etc.) and one or more
  vendored library headers. No shared internal state.

Conclusion: the five files are independently deployable.

---

## Option (a): All-In Single TU

Pull all five protocol `.cc` files and their entire vendored C dependency
sources into `csp.cpp`. The amalgamation script would need to:

1. Inline C sources (llhttp, nghttp2, wslay, nghttp3, ngtcp2) as
   `extern "C" { … }` blocks, since they are C99.
2. Either commit the three generated version headers or generate them as
   part of `make dist`.
3. Inline `ngtcp2_crypto_picotls_minicrypto.c` in the same `extern "C"` block.

**Size estimate:**

| Component | Lines |
|---|---|
| Current `dist/csp.cpp` | 7,101 |
| Five protocol `.cc` files | 3,902 |
| llhttp C sources | 11,688 |
| nghttp2 C sources | 23,575 |
| wslay C sources | 1,993 |
| nghttp3 C sources | 17,966 |
| ngtcp2 lib C sources | 30,960 |
| ngtcp2 crypto/picotls + adapter | 3,253 |
| **Estimated total `csp.cpp`** | **~100,000 lines** |

**Pros:**
- Dist remains three files — zero API change for users.
- `make test-dist` unchanged; just stop suppressing protocol tests.

**Cons:**
- `csp.cpp` balloons from 7K to ~100K lines — a 14× increase. Every dist
  consumer's build now compiles the full QUIC stack even if they only use
  channels.
- The amalgamation script becomes significantly more complex: it must handle
  C-in-C++ embedding (`extern "C"` blocks), C99-vs-C++20 include ordering,
  and the Makefile's `NGTCP2_CFLAGS` (`-DHAVE_ARPA_INET_H=1` etc.) that are
  currently applied only to C sources.
- Maintenance cost: every vendored library update must re-verify that
  `extern "C"` wrapping still compiles cleanly.
- Users who only want WebSocket are still paying to compile QUIC and HTTP/2.
- Build time impact: a clean build of the dist by a user currently compiles
  one ~7K-line file. Option (a) turns that into compiling ~100K lines in a
  single TU — slow even with modern hardware, and no incremental benefit from
  partial rebuilds inside the file.
- The `CSP_TLS` guard makes QUIC optional, but HTTP, HTTP/2, and WebSocket
  are always included regardless of user intent.

---

## Option (b): Separate `csp_protocols.cpp`

Add a fourth dist file — `dist/csp_protocols.cpp` — containing the five
protocol `.cc` files interleaved with their vendored C sources (also
`extern "C"` wrapped). Users opt in by compiling this extra TU; the core
three-file bundle is unchanged.

**Size estimate:**

| File | Lines |
|---|---|
| `dist/csp.cpp` | 7,101 (unchanged) |
| `dist/csp_globals.cpp` | 121 (unchanged) |
| `dist/csp.h` | 8,796 (unchanged) |
| `dist/csp_protocols.cpp` (new) | ~93,000 |

**Pros:**
- Core dist users (channels, cancellation, parts, TLS) pay zero compile cost
  for protocols they don't use.
- Clean opt-in: users who want protocols add exactly one file to their build.
- Mirrors how the non-dist build works: the Makefile compiles protocol sources
  only for users who link the protocol TUs.
- `amalgamate.py` changes are contained to a new code path, leaving existing
  logic untouched.
- Future protocols (e.g. SSH, gRPC over HTTP/2) slot naturally into
  `csp_protocols.cpp`.

**Cons:**
- Four-file dist instead of three — documentation and user instructions change.
- `make test-dist` needs a variant that links `csp_protocols.cpp` for the
  protocol test suite (currently filtered out).
- The same `extern "C"` and C99-flag complexity applies, just in the new file
  rather than in `csp.cpp`.

---

## Option (c): Per-Protocol Dist Files (Rejected)

Ship `dist/csp_http.cpp`, `dist/csp_http2.cpp`, etc. Finer opt-in, but five
extra files with five separate entry points for users and five separate
`make test-dist` variants. The complexity cost exceeds the benefit given that
the five files compile in seconds anyway and the common case is "want all or
none". Rejected in favour of option (b).

---

## The `extern "C"` Problem

All five vendored libs are C99. Inlining their `.c` sources into a C++ TU
requires wrapping them:

```cpp
extern "C" {
#include "llhttp.c"
/* … */
}
```

This works in practice (CSP already compiles the vendored C sources as
separate `.o` files without issue), but it creates a single preprocessor
namespace. The biggest risk is duplicate symbol collisions between ngtcp2 and
nghttp3, which both bundle `sfparse` — a problem the Makefile already
resolves by excluding nghttp3's copy at link time. An amalgamation must
replicate this exclusion.

An alternative that avoids the `extern "C"` complexity: keep the C vendored
sources as **separate `.c` files** shipped alongside `csp_protocols.cpp`.
The user adds both `csp_protocols.cpp` and `vendor/*.c` to their build.
This is closer to what users do today when they vendore ngtcp2 themselves and
is arguably cleaner — but it inflates the "dist" concept from a handful of
files to a directory tree, which is a different class of change.

---

## `version.h` Status

The ngtcp2 `version.h` is already generated and committed to the worktree
(`vendor/github.com/ngtcp2/ngtcp2/lib/includes/ngtcp2/version.h` contains
`"1.22.0"` / `0x011600`). The Makefile regenerates it from `.h.in` as a
make dependency, but the committed copy is stable.

For the dist, `amalgamate.py` would reference the committed `version.h`
directly — no `sed` step needed during `make dist`. The same applies to
nghttp2 and nghttp3 (their `.h.in` templates have the same two-variable
substitution pattern; all three can be committed as generated artifacts).

Action item: **commit `vendor/.../nghttp2ver.h` and `vendor/.../nghttp3/version.h`**
as part of the implementation PR, mirroring what's already done for ngtcp2.

---

## Recommendation: Option (b)

Ship `dist/csp_protocols.cpp` as a fourth dist file.

**Rationale:**
- Option (a) creates a 100K-line monolith that punishes every dist user
  (even those who only use channels) with protocol-library compile cost.
  That directly contradicts CSP's "vendor three files" user story.
- Option (b) preserves the core three-file story while adding a clean,
  documentable opt-in path. Users who want QUIC or HTTP add one file.
- The `extern "C"` complexity is the same in both options; it's not an
  argument for (a).
- The sfparse deduplication issue is real and must be solved regardless of
  option; in (b) it affects only `csp_protocols.cpp`.

**What `make test-dist` needs:**

```make
test-dist: dist
    # Core dist (no protocols)
    $(MAKE) CSP_INCLUDE=dist CSP_TLS=1 test
    $(MAKE) CSP_INCLUDE=dist CSP_TLS=0 test
    # Protocols dist (with csp_protocols.cpp)
    $(MAKE) CSP_INCLUDE=dist CSP_TLS=1 CSP_PROTOCOLS=1 test
```

Where `CSP_PROTOCOLS=1` adds `dist/csp_protocols.cpp` to `LIB_SRCS` and
removes the protocol test suppressions.

---

## Open Questions for the User

1. **`extern "C"` vs. separate C files in dist?**
   Option (b) as described inlines C sources into a C++ TU. The alternative
   is shipping `dist/` as a small directory with `csp_protocols.cpp` plus
   the vendored C sources (llhttp, nghttp2, etc.) as `.c` files alongside it.
   Which is preferred — single-file opt-in or directory-of-files opt-in?

2. **Scope: all five protocols or QUIC only?**
   🎯T22's acceptance criteria mentions QUIC specifically. Pulling all five
   protocols into `csp_protocols.cpp` is the principled answer, but it also
   means committing nghttp2ver.h and nghttp3/version.h and solving the
   sfparse deduplication problem in the amalgamation. Is it acceptable to
   start with just `quic.cc` + ngtcp2 in this PR, leaving the other four for
   a follow-up?

3. **sfparse deduplication strategy?**
   nghttp2 and nghttp3 both bundle `sfparse.c`. If both are inlined into one
   TU, one copy must be suppressed. Options: (i) `#define` guard wrapping one
   copy, (ii) `#undef` trickery, (iii) only include the nghttp2 copy and
   rely on the linker (as the Makefile does today). Option (iii) works in
   a single TU only if both copies are actually identical — the Makefile
   comment says "minor version drift but compatible ABI", which implies they
   may differ. Verify before implementing.

4. **Naming?**
   `csp_protocols.cpp` is clear but long. `csp_proto.cpp` is shorter.
   `csp_net.cpp` would be more specific. User preference?
