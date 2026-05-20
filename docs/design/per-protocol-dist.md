# Per-protocol dist drop-in with linker dead-code elimination

**Target**: 🎯T23

**Status**: Design + initial file-split implementation (this document drives the
work).

## Background

CSP has historically distributed as three vendor-drop-in files:

| File | Contents |
|---|---|
| `dist/csp.h` | Single header — public API + internals |
| `dist/csp.cpp` | Amalgamated implementation + fcontext assembly |
| `dist/csp_globals.cpp` | Thread-local state (separate TU; see [`docs/tls-caching-bug.md`](../tls-caching-bug.md)) |

The three files are generated from `src/` and `include/` by
`scripts/amalgamate.py` (`make dist`).

When CSP grew network protocols (TLS, HTTP/1.1, HTTP/2, HTTP/3, WebSocket,
QUIC), each protocol pulled in a vendored C99 library:

| Protocol | Vendored library |
|---|---|
| TLS | PicoTLS + minicrypto |
| HTTP/1.1 | llhttp |
| HTTP/2 | nghttp2 |
| WebSocket | wslay |
| QUIC | ngtcp2 + PicoTLS |
| HTTP/3 | nghttp3 (on top of QUIC) |

Folding all of those into the single `dist/csp.cpp` would force every CSP user
to compile and link several megabytes of unrelated protocol code (and depend on
six third-party libraries) even if they only need channels and scheduling.

The original 🎯T22 plan was to merge everything (the protocol implementations
*and* the C99 vendored deps) into one giant `dist/csp_net.cpp`. That hit a
hard C-vs-C++ wall: `extern "C"` controls linkage, not parsing, so C99 idioms
(implicit `void*` → typed pointer conversions, `int` → enum, header guard
collisions across libraries) produce ~600 hard Clang errors in C++20 mode.
🎯T22 was retired in favour of this design.

## Design: per-protocol .cpp files + linker dead-code elimination

The chosen approach distributes implementations as **one .cpp file per
protocol**, alongside the existing core `csp.cpp` + `csp_globals.cpp`:

```
dist/csp.h            single header (all public API + internals)
dist/csp.cpp          core: runtime, channels, scheduler, reactor, io, net, …
dist/csp_globals.cpp  thread-local state (separate TU for TLS correctness)
dist/csp_tls.cpp      TLS 1.3 via PicoTLS  (needs picotls in link line)
dist/csp_http.cpp     HTTP/1.1 via llhttp  (needs llhttp)
dist/csp_http2.cpp    HTTP/2 via nghttp2   (needs nghttp2, optionally picotls)
dist/csp_ws.cpp       WebSocket via wslay  (needs wslay)
dist/csp_quic.cpp     QUIC via ngtcp2      (needs ngtcp2 + picotls)
dist/csp_http3.cpp    HTTP/3 via nghttp3   (needs nghttp3 + ngtcp2 + picotls)
```

A user that wants channels + HTTP/1.1 compiles four CSP files
(`csp.cpp`, `csp_globals.cpp`, `csp_http.cpp`) and links the llhttp objects.
No other protocol .cpp file is built; no other third-party library is linked.

### How dead-code elimination keeps the link minimal

The mechanism rests on three orthogonal compiler / linker tools:

1. **Per-TU drop**. Static archives (`libcsp.a`) only include TUs that
   contribute symbols the link needs. A user that never references
   `csp::http::serve` never has `csp_http.o` pulled into the link. Since
   `csp.cpp` references no `csp::http` symbol, only the protocol .cpp file
   keeps the protocol live.
2. **Per-function drop**. When CSP is compiled with `-ffunction-sections`
   `-fdata-sections` and linked with `-Wl,--gc-sections` (Linux/Windows-ld) or
   `-Wl,-dead_strip` (ld64 / macOS), the linker can drop **individual
   functions** within a TU when no symbol in the TU is referenced. This means
   even if the user compiles `csp_http.cpp` into the link, calling no `http::`
   symbol leaves the entire TU as dead bytes that the linker strips.
3. **Inter-protocol references resolve naturally**. `csp::quic::listen` is
   implemented in `csp_quic.cpp` and contains direct calls to PicoTLS. The
   linker sees those references and pulls in `csp_tls.cpp` (or the PicoTLS
   objects directly). Similarly, `csp::http2::serve_tls` references
   `csp::tls::context`, which pulls in `csp_tls.cpp`. Users do not need to
   know the protocol dependency graph — calling the top-level entry point is
   sufficient.

### The five DCE rules

The model above only works if the front-door TU (`csp.cpp`) and the public
header (`csp.h`) do not introduce hidden references to protocol-specific
symbols. The following five rules express the contract.

#### Rule 1 — No protocol enums in the front door

A `protocol_t` enum with `protocol_t::http`, `protocol_t::http2`, etc. forces
any switch over the enum to reference every protocol's entry point, defeating
DCE.

**Forbidden:**

```cpp
enum class protocol { http, http2, http3, ws, quic };
void serve(uint16_t port, protocol p) {
    switch (p) {
    case protocol::http:  return http::serve(port);   // forces http link
    case protocol::http2: return http2::serve(port);  // forces http2 link
    // …
    }
}
```

**Preferred:** direct calls per protocol (`csp::http::serve(port)`,
`csp::http2::serve(port)`) — each in its own TU.

#### Rule 2 — No protocol-specific methods on shared types

Adding `csp::net::connection::upgrade_to_http2()` would require
`<csp/http2.h>` (and a forward declaration of HTTP/2 internals) to be visible
inside `<csp/net.h>`. That pulls the HTTP/2 type system into the front door
and creates symbol references back to the HTTP/2 TU.

**Forbidden:**

```cpp
// In csp/net.h:
struct connection {
    // …
    http2::session upgrade_to_http2();  // pulls http2 into net’s public surface
};
```

**Preferred:** upgrade lives in the protocol namespace. `csp::http2::upgrade(net::connection&&)` is implemented in `csp_http2.cpp`; `net::connection` knows nothing about it.

#### Rule 3 — No static registration in protocol TUs

A static constructor in `csp_http.cpp` that registers HTTP with a central
registry forces the TU to be live (linker cannot drop a TU that has a static
initialiser with observable side effects).

**Forbidden:**

```cpp
// In csp_http.cpp:
static struct register_t {
    register_t() { csp::net::register_protocol("http", &http_factory); }
} _;
```

**Preferred:** factories are explicit. The user that wants HTTP calls
`csp::http::serve(...)` directly (or, in the forward-looking API, passes
`csp::http::enable()` as an option to `csp::net::serve(...)`). No
side-effecting global initialisers.

#### Rule 4 — No central virtual base with per-protocol subclasses

If `net::serve` returns a `unique_ptr<protocol_server>` and each protocol
defines a subclass, the vtable of the abstract base accumulates vptrs to all
concrete subclasses through the typeinfo / RTTI graph, and Whole-Program
optimisation can't always tell which subclasses are live. In practice,
RTTI references in the base TU make every subclass TU live.

**Forbidden:**

```cpp
// In csp/net.h:
struct protocol_server {
    virtual ~protocol_server() = default;
    virtual void serve() = 0;
};

// In csp_http.cpp:
struct http_server : protocol_server { void serve() override; };
// In csp_http2.cpp:
struct http2_server : protocol_server { void serve() override; };
```

**Preferred:** each protocol exposes its own concrete `server` type. No
common base; no RTTI fan-out.

#### Rule 5 — Front-door TU references no protocol-specific symbols

The implementation TU of `csp::net` (and `csp::io`, `csp::runtime`, the
scheduler, etc.) must not contain a single reference to a protocol-specific
function or variable. If it does, that protocol becomes a transitive
dependency of every CSP user, channels-only included.

**Forbidden:** an `#ifdef` block in `src/net.cc` that selects between TLS and
plain-text accept depending on whether TLS is compiled in. Even guarded by
`#ifdef CSP_TLS`, build-system mistakes can wire it in. Better: keep
`src/net.cc` zero-aware of TLS and let `csp_tls.cpp` provide TLS-augmenting
wrappers.

This rule is the easiest to violate accidentally. We enforce it by:

1. The amalgamation script excludes protocol .cc files from `dist/csp.cpp` —
   any new file added under `src/` that wants TLS or HTTP must be either
   gated into a per-protocol .cpp or excluded from the front-door TU.
2. [`scripts/lint_frontdoor.py`](../../scripts/lint_frontdoor.py) (🎯T23.4)
   greps `dist/csp.cpp` and every `src/` file that amalgamates into it for
   `csp::tls::`, `csp::http::`, `csp::http2::`, `csp::http3::`, `csp::ws::`,
   `csp::quic::` references. The lint strips comments before scanning so
   docstrings referencing a protocol API don't trip it. Wired into `make`
   (every build runs the lint; failure aborts the build with a message
   naming the violated rule) and into `make bullseye` (the standing-
   invariants check). Catches violations at source-edit time, not at
   `make dist` time.

### Forward-looking factory API: `csp::<proto>::enable()`

The acceptance criteria for 🎯T23 mention a factory-function API of the form
`csp::http::enable()` that returns a type-erased option struct, consumable by
`csp::net::serve(port, {tls::enable(...), http::enable()})`.

That unified-server entry point is a **forward-looking API** kept separate
from the immediate file-split deliverable. The migration plan:

1. **Phase A (now)**: Land the per-protocol .cpp file split. The existing
   direct-call API (`csp::http::serve`, `csp::tls::context`, …) continues to
   work unchanged; per-function DCE handles the no-call case. Document the
   five rules and gate them with code review + future lint.
2. **Phase B (initial release — 🎯T23.1)**: Introduce the `enable()`
   factory pattern + a unified
   `csp::net::serve(uint16_t port, std::initializer_list<csp::net::protocol_option>)`
   entry point. Each `enable()` factory lives in its own protocol TU and
   returns an opaque `protocol_option` (a `void* config` + a function-
   pointer `apply` + an optional `destroy`). The unified `serve` walks the
   option list and calls each `apply()`; each apply starts its protocol's
   server (delegating to the existing direct-call API) and pushes the
   typed server handle into the returned
   `csp::net::server::protocol_servers` (a `std::vector<std::any>`).
   Critically: the unified `serve` lives in `dist/csp.cpp` (the front-door
   TU) and references no protocol symbols by name — only via the type-
   erased function pointers carried in the `protocol_option`s.

   Phase B ships in a minimum-viable shape: single-protocol case (e.g.
   `csp::net::serve(8080, {csp::http::enable()})`) is fully wired. The
   only protocol with a non-stub `enable()` in the initial drop is
   `csp::http`; other protocols' factories will land as they're wired
   through. ALPN negotiation across multiple options (TLS + ALPN
   choosing between HTTP/1.1 and HTTP/2 on the same socket) is the
   most consequential follow-up — it requires the TLS option's apply
   to install an ALPN callback that consults the other applied options'
   advertised protocol names, which is more careful than apply() running
   in isolation. Filed for the design pass when a real user pulls.

The reason for splitting Phase A and Phase B: the file split is mechanical
and unblocks downstream users immediately. The unified-server API needs more
careful design (interaction with ALPN negotiation, how `http2::serve_tls`'s
existing config maps onto the new factory shape, what happens when
incompatible options are combined, etc.). Doing both in one pass would either
slip the file split or rush the API design.

## Initial file layout (Phase A)

Generated by `scripts/amalgamate.py`:

```
dist/
├── csp.h                  (all headers; protocol APIs gated behind #ifdef as today)
├── csp.cpp                (core: runtime/channels/scheduler/reactor/io/net/blocking/signal/timer/cancel/…)
├── csp_globals.cpp        (thread-local state — separate TU)
├── csp_tls.cpp            (src/tls.cc; needs CSP_TLS=1 and PicoTLS in link)
├── csp_http.cpp           (src/http.cc; needs llhttp in link)
├── csp_http2.cpp          (src/http2.cc; needs nghttp2 in link; TLS optional via CSP_TLS)
├── csp_ws.cpp             (src/ws.cc; needs wslay in link)
├── csp_quic.cpp           (src/quic.cc + src/ngtcp2_crypto_picotls_minicrypto.c; needs CSP_TLS=1, ngtcp2, PicoTLS)
└── csp_http3.cpp          (src/http3.cc; needs nghttp3 + ngtcp2 + PicoTLS; CSP_TLS=1)
```

Each per-protocol .cpp file begins with `#include "csp.h"` and contains
**only** that protocol's implementation (and, for QUIC, the picotls/ngtcp2
adapter glue). No protocol .cpp file references another protocol's
implementation TU directly — they reference each other only through the
public `csp::<proto>::` API, which the linker resolves through the protocol
TU's exported symbols.

### Compile / link recipes

**Channels only:**

```bash
c++ -std=c++20 -ffunction-sections -fdata-sections \
    -c csp.cpp csp_globals.cpp
c++ -Wl,-dead_strip   csp.o csp_globals.o -o app    # macOS
c++ -Wl,--gc-sections csp.o csp_globals.o -o app    # Linux / lld
```

**Channels + HTTP/1.1:**

```bash
c++ -std=c++20 -DCSP_TLS=0 \
    -ffunction-sections -fdata-sections \
    -I vendor/llhttp/include \
    -c csp.cpp csp_globals.cpp csp_http.cpp \
       vendor/llhttp/src/{llhttp,api,http}.c
c++ -Wl,-dead_strip csp.o csp_globals.o csp_http.o {llhttp,api,http}.o -o app
```

**Channels + HTTPS (TLS + HTTP/1.1):**

```bash
c++ -std=c++20 -DCSP_TLS=1 \
    -ffunction-sections -fdata-sections \
    -I vendor/picotls/include -I vendor/llhttp/include \
    -c csp.cpp csp_globals.cpp csp_tls.cpp csp_http.cpp \
       vendor/picotls/lib/*.c vendor/llhttp/src/{llhttp,api,http}.c
c++ -Wl,-dead_strip csp.o csp_globals.o csp_tls.o csp_http.o picotls_*.o llhttp_*.o -o app
```

`scripts/vendor-deps.sh` (follow-up sub-target) will automate the vendored
library compilation with per-protocol flags.

## Interaction with `make test-dist`

The Makefile's `test-dist` recursive build will be taught to:

* Pick up all `dist/csp*.cpp` files (not just the core trio).
* Re-enable the previously-excluded test files (`net.test.cc`, `http.test.cc`,
  `http2.test.cc`, `http3.test.cc`, `ws.test.cc`, `quic.test.cc`) since the
  dist now ships every protocol implementation.
* Continue to honour `CSP_TLS=0`, in which case TLS-dependent protocol .cpp
  files (csp_tls, csp_quic, csp_http3) are skipped from the build; tests that
  require those features are likewise skipped.

The per-protocol CI matrix (build channels-only, http-only, http+ws,
quic-only, full stack and verify the link line) is a follow-up sub-target.

## Follow-up sub-targets

The full 🎯T23 acceptance criteria include items beyond Phase A:

* **Sub-target A** (Phase B): unified `csp::net::serve(opts)` + per-protocol
  `enable()` factories. Requires API design and consumer migration plan.
* **Sub-target B**: `scripts/vendor-deps.sh` that fetches and builds llhttp,
  nghttp2, nghttp3, wslay, ngtcp2, PicoTLS into `vendor/` with per-protocol
  feature flags (`--http`, `--http2`, `--quic`, `--all`).
* **Sub-target C**: CI matrix that builds the documented subset
  configurations (channels-only, http-only, http+ws, quic-only, full stack)
  and checks the link line / final binary for the absence of unselected
  protocol libraries. Requires sub-target B to be in place first.
* **Sub-target D**: a build-time lint that scans `dist/csp.cpp` for any
  `csp::tls::`, `csp::http::`, `csp::http2::`, `csp::http3::`, `csp::ws::`,
  or `csp::quic::` references and fails the build if any are present. The
  front-door TU must remain protocol-agnostic.

These four can be sequenced independently once Phase A is in. None blocks
Phase A.
