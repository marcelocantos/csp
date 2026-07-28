# CLAUDE.md

## Project Overview

CSP is a C++ imp-based concurrency library with typed, synchronous channels
based on Communicating Sequential Processes. Namespace: `csp`.

## Task Tracking

Followable work is tracked as bullseye targets in `bullseye.yaml`
(not `docs/todo.md`).

## Release

homebrew_tap: disabled

CSP is a C++ source library distributed as three files in `dist/` — users
vendor them into their own project. There is no binary to install, so the
release skill's Homebrew tap plumbing does not apply.

## Gates

profile: csp

The `csp` gate profile (`~/.claude/gates/csp.yaml`, merged over
`base.yaml`) adds a **`windows-vm-validated`** pre-merge gate: the
**full** Windows build + test suite is validated on the local Parallels
VM (`ssh hms-vm`, Win11 ARM64) by `scripts/win-validate.sh`.

Cloud `Windows x86_64` still runs on every PR: MSVC build + an
**abbreviated smoke** (`scripts/win-ci-smoke.ps1` — core channel/runtime
units only, seconds of test time). It is **not** a required master
ruleset check (merge/release wait on macOS/Linux/TLA+). The full suite
is the local gate only so cloud runners stay fast; 🎯T39 (listen hang)
was fixed in v0.27.0 (`FD_ACCEPT` on the Windows reactor).

Flow is **push-first**: push the branch, then run
`scripts/win-validate.sh` (it validates the *pushed* commit), which
builds with VS/MSVC ARM64 on the VM and runs full `csp_tests.exe`.
One-time VM provisioning: Visual Studio 18 Community with ARM64 C++
tools + the bundled CMake, Git.

## Build System

```bash
make        # build and run all tests
make build  # compile only
make dist   # generate distribution files (dist/)
make iwyu   # remove unused includes (clang-tidy misc-include-cleaner)
make clean  # remove build/
```

Build artifacts go to `build/`. Compiler: Clang, C++20, libc++, `-O2 -g`.

## C++20 Style

- **`requires` over `std::enable_if_t`**: Use `requires` clauses or concepts
  for SFINAE constraints.
- **Stored `F` template parameter** over `std::function` when the callable is
  only moved and invoked, never copied (callbacks, one-shot tasks, RAII
  cleanup — e.g. `OnScopeExit`).
- **`<bit>` header**: `std::bit_ceil` over hand-rolled `round_up_pow2`,
  `std::popcount` over `__builtin_popcount`.
- **Spaceship operator (`<=>`)** and defaulted `operator==` for comparison
  boilerplate.
- **`[[nodiscard]]`** on functions whose return values must not be ignored.

## Architecture

All code lives in `namespace csp`. Internal implementation details live in
`namespace csp::internal` (type-erased channel operations, scheduler
primitives) and `namespace csp::detail` (runtime, processor, channels, reactor,
blocking pool, stack pool, HAMT, and other implementation machinery).

### Distribution

CSP is distributed as a small set of vendor-drop-in files in `dist/`. The
**core** trio is always required:

| File | Contents |
|---|---|
| `csp.h` | Single header (all public API, combinators, internals) |
| `csp.cpp` | Core implementation + fcontext inline assembly (no protocol code) |
| `csp_globals.cpp` | Thread-local state (must be a separate TU — see `docs/tls-caching-bug.md`) |

Each network protocol ships as its own optional drop-in `.cpp` (🎯T23):

| File | Protocol | Vendored library | TLS-gated |
|---|---|---|---|
| `csp_tls.cpp` | TLS 1.3 | PicoTLS + minicrypto | yes (`CSP_TLS`) |
| `csp_http.cpp` | HTTP/1.1 | llhttp | no |
| `csp_http2.cpp` | HTTP/2 | nghttp2 | partial (`serve_tls`) |
| `csp_ws.cpp` | WebSocket | wslay (+ llhttp via http drop-in) | no |
| `csp_quic.cpp` | QUIC | ngtcp2 + PicoTLS + `ngtcp2_crypto_picotls_minicrypto.c` | yes |
| `csp_http3.cpp` | HTTP/3 | nghttp3 (+ ngtcp2, PicoTLS) | yes |

Users cherry-pick the protocols they need: compile `csp.cpp` + `csp_globals.cpp` +
the chosen `csp_<proto>.cpp` files, and link the corresponding vendored
libraries. With `-ffunction-sections -fdata-sections` + `-Wl,-dead_strip`
(macOS) or `-Wl,--gc-sections` (Linux), the linker drops unreferenced TUs and
functions, including any unused protocol code that ended up in the link by
accident. See [`docs/design/per-protocol-dist.md`](docs/design/per-protocol-dist.md)
for the five DCE rules that make this model contractual:

1. No protocol enums in the front door.
2. No protocol-specific methods on shared types.
3. No static registration in protocol TUs.
4. No central virtual base with per-protocol subclasses.
5. The front-door TU (`csp.cpp`) references no protocol-specific symbols.

All dist files are generated from `src/`/`include/` by `scripts/amalgamate.py`
(`make dist`). The QUIC adapter `ngtcp2_crypto_picotls_minicrypto.c` is a
C99 TU; the script copies it verbatim into `dist/` since it cannot be
amalgamated into C++ alongside the other protocol implementations.

External users fetch the third-party libraries via
[`scripts/vendor-deps.sh`](scripts/vendor-deps.sh), which clones llhttp,
picotls, nghttp2, nghttp3, ngtcp2, and wslay at pinned commits into
`vendor/<name>/` and generates the autotools-style version headers.
Per-protocol flags (`--http`, `--http2`, `--http3`, `--ws`, `--quic`,
`--tls`, `--all`) match the drop-in `.cpp` files. The script is the
canonical answer to "how do I get the deps the dist files need?" and is
referenced from `dist/AGENTS-CSP.md`.

### Development source layout

- **include/csp/csp.h** — Core API: `spawn`, `await_completion`, `yield`, `chan`,
  `writer`, `reader`, `alt`/`prialt`, `chan_op`, `csp::internal` type-erased API.
- **include/csp/timer.h** — Timer primitives (`sleep`, `after`, `tick`).
- **include/csp/io.h** — Non-blocking I/O (kqueue reactor, DNS resolution).
- **include/csp/cancel.h** — Cooperative cancellation (`cancel_guard`,
  `done`, `timed_out`).
- **include/csp/tls.h** — TLS 1.3 via PicoTLS (`context`, `conn`), behind
  `#ifdef CSP_TLS`.
- **include/csp/http.h** — HTTP/1.1 server (`serve`, `request`, `response`,
  `endpoint`). Implementation in `src/http.cc` with vendored llhttp.
- **include/csp/net.h** — TCP networking (`listen`, `dial`, `connection`).
- **include/csp/signal.h** — Unix signal channels.
- **include/csp/blocking.h** — Blocking thread pool.
- **include/csp/dynamic.h** — `dynamic<T>` dynamic-scoped variables (HAMT).
- **include/csp/part/** — 70+ stream combinators (`filter`, `producer`,
  `consumer` with `operator|` composition).
- **include/csp/internal/** — Imp struct, runtime, processor,
  stack pool, HAMT, reactor, blocking pool, signal types.
- **src/** — Implementation files (`csp.cc`, `channel.cc`, `runtime.cpp`,
  `csp_globals.cpp`, `reactor.cc`, `blocking_pool.cc`, `signal.cc`,
  `stack_pool.cc`, `hamt.cc`, `stack_analysis_arm64.cc`, `log.cc`,
  `cancel.cc`, `timer.cc`, `io.cc`, `clock.cc`, `tls.cc`, `net.cc`,
  `http.cc`).

### Stream combinator conventions

- **Parameterless parts are variable templates, not functions.** A part that
  takes no runtime arguments should be declared as
  `template <typename T> inline auto const foo = make_filter<T>(...);`, not as
  a function returning the filter. Users write `foo<int>` not `foo<int>()`.
  Exception: parts that need local type aliases or `static_assert` (e.g.
  `nwise`) may remain functions.
- **Always add documentation when implementing a new part.** Create a detail
  page in `docs/reference/parts/<name>.md`, add an entry to the catalog table
  in `docs/reference/parts.md`, and add a row to the Combinator Reference table
  in `dist/AGENTS-CSP.md`.

### Key design points

- **Per-endpoint lifecycle**: Channels have independent write/read endpoint
  refcounts. Either end can be closed independently, and endpoint death is
  observable via `alt`/`prialt`.
- **M:N threading**: Imps are multiplexed across OS threads via a
  work-stealing scheduler. The runtime auto-initializes with hardware
  concurrency on first use. Override with `set_maxprocs(n)` or
  `CSP_MAXPROCS` env var. Use `set_maxprocs(1)` for single-threaded mode.
- **Type-erased compilation firewall**: Templates in the header dispatch
  through `csp::internal` (opaque `WriterRef`/`ReaderRef`/`ChanOp` types
  with `void*` pointers), keeping complex channel/scheduler logic in `.cc`
  files.
- **chan_op RAII**: Channel operations return `chan_op<T>` objects whose
  destructors call `prialt`, making `w << val;` block as a statement.
  Two-phase protocol: `prialt_begin` finds a match with locks held, typed
  transfer happens inline, then `alt_end` unlocks and schedules.

## Documentation

When making code changes, keep the following documentation in sync:

- **`dist/AGENTS-CSP.md`** — Token-efficient reference for coding agents.
  Update the Combinator Reference table when adding/removing/renaming parts.
  Update API sections when core types or functions change. This file is
  maintained directly (not generated).
- **`docs/reference/parts.md`** — Parts catalog with per-section tables.
  Add new parts to the appropriate category table, linked to their detail page.
- **`docs/reference/parts/<name>.md`** — Per-part detail page. Create one for
  every new part. Use an existing page as a template. Standard structure:
  - **Heading**: `# name` — one-paragraph description.
  - **Signature**: fenced C++ block with template signature and return type.
  - **Parameters** (if any): table with type and description.
  - **Topology**: Mermaid `graph LR` showing input/output channels and the
    internal imp. Add a `stateDiagram-v2` when the part has non-trivial
    lifecycle states.
  - **Semantics**: bulleted list covering backpressure, exit conditions, edge
    cases (empty input, output death, input close).
  - **Example**: minimal `#include "csp.h"` snippet. Additional sub-examples
    for notable use cases.
  - **See Also**: links to related parts.
- **`docs/guide/`** — Narrative guide chapters. Update the relevant chapter
  when behaviour changes (e.g., `03-multiplexing.md` for alt/prialt changes,
  `05-combinators.md` for part system changes).
- **`docs/reference/`** — Per-topic reference pages (channels, scheduling,
  timers, I/O, signals, blocking, dynamic scoping, combinators, multiplexing).
  Update when the corresponding API surface changes.
- **`include/csp.h`** — Gateway header. Add an `#include` when creating a
  new public header.

`make` runs `scripts/check_md_links.py` which verifies all markdown
cross-references resolve. Broken links fail the build.

## Debugging hard concurrency bugs

When a concurrency bug resists direct investigation (no clear repro,
flaky, timing-dependent), write a paper stub in `docs/papers/` before
reaching for systematic tools (ASan, TLA+, etc.):

1. **Enumerate the actors** as a numbered sequence of steps. Force
   yourself to name every transition ("catches → writes to channel"
   not "handles the exception"). Gaps between sub-steps are where
   bugs hide.
2. **State an explicit hypothesis**, even if you suspect it's wrong.
   A wrong hypothesis that names the right code is a compass bearing.
3. **Name the invariant** you believe is being violated ("stack not
   freed while X references it").
4. **Sleep on it.** The paper stub is most valuable when re-read with
   fresh eyes — the structured trace often makes the bug visible to
   a reader in a way it wasn't to the writer mid-investigation.

This process complements TLA+: the paper scopes the state space and
actors that a spec would model. Sometimes the scoping itself reveals
the bug, making the spec post-hoc documentation rather than a
diagnostic tool.

## Formal Verification (`formal/`)

TLA+ specs verify concurrent protocols. Run with `./formal/tlc SpecName`.

- **After fixing a concurrency bug**: write `Foo.tla` (fixed) + `Foo_Bug.tla`
  (buggy, expected to violate invariant) pair. Both get `.cfg` files.
- **When writing concurrent decision points** (loop exits, irreversible state
  transitions based on shared variables): write the safety invariant first
  ("this exit is only safe when X"), model what can invalidate X, run TLC
  before writing the C++.
- **When modifying concurrent code**: check if a `formal/` spec covers that
  protocol and update it to match.
- **Keep specs in sync**: if the C++ changes the variables or ordering in a
  modeled protocol, update the corresponding `.tla` file.

## Tests

doctest (vendored in `vendor/include/doctest/`). Test files in `test/`
with `.test.cc` extension.

## Dependencies

- **Boost.Context** (vendored as git submodule in
  `vendor/github.com/boostorg/context/`; only the fcontext assembly files are
  compiled)
- **PicoTLS** (vendored as git submodule in
  `vendor/github.com/h2o/picotls/` with minicrypto backend; compiled when
  `CSP_TLS=1`, which is the default; TLS 1.3 only, no OpenSSL dependency)
- **llhttp** (vendored in `vendor/github.com/nodejs/llhttp/`; MIT licence;
  HTTP/1.1 parser from Node.js; compiled as part of the normal build;
  excluded from the dist amalgamation)
- **doctest** (vendored, header-only)

### Optional tools

- **clang-tidy**: Used by `make iwyu` (include cleaner). Ships with Xcode
  Command Line Tools; standalone: `brew install llvm`.
