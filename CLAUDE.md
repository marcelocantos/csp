# CLAUDE.md

## Project Overview

CSP is a C++ microthreading library with typed, synchronous channels based on
Communicating Sequential Processes. Namespace: `csp`.

## Build System

```bash
make        # build and run all tests
make build  # compile only
make amalg  # generate amalgamation files (amalg/)
make iwyu   # remove unused includes (clang-tidy misc-include-cleaner)
make clean  # remove build/
```

Build artifacts go to `build/`. Compiler: Clang, C++17, libc++, `-O2 -g`.

## Architecture

All code lives in `namespace csp`. Internal implementation details live in
`namespace csp::internal` (type-erased channel operations, scheduler
primitives) and `namespace csp::detail` (runtime, processor, channels, reactor,
blocking pool, stack pool, HAMT, and other implementation machinery).

### Amalgamated distribution

CSP is distributed as three files in `amalg/`:

| File | Contents |
|---|---|
| `csp.h` | Single header (all public API, combinators, internals) |
| `csp.cpp` | All implementation source + fcontext inline assembly |
| `csp_globals.cpp` | Thread-local state (must be a separate TU — see `docs/amalg-tls-bug.md`) |

These are generated from the development sources by `scripts/amalgamate.py`
(`make amalg`).

### Development source layout

- **include/csp/csp.h** — Core API: `spawn`, `schedule`, `yield`, `chan`,
  `writer`, `reader`, `alt`/`prialt`, `chan_op`, `csp::internal` type-erased API.
- **include/csp/timer.h** — Timer primitives (`sleep`, `after`, `tick`).
- **include/csp/io.h** — Non-blocking I/O (kqueue reactor, DNS resolution).
- **include/csp/signal.h** — Unix signal channels.
- **include/csp/blocking.h** — Blocking thread pool.
- **include/csp/dynamic.h** — `dynamic<T>` dynamic-scoped variables (HAMT).
- **include/csp/part/** — 50+ stream combinators (`filter`, `producer`,
  `consumer` with `operator|` composition).
- **include/csp/internal/** — Microthread struct, runtime, processor,
  stack pool, HAMT, reactor, blocking pool.
- **src/** — Implementation files (`csp.cc`, `channel.cc`, `runtime.cpp`,
  `csp_globals.cpp`, `reactor.cc`, `blocking_pool.cc`, `signal.cc`,
  `stack_pool.cc`, `hamt.cc`, `stack_analysis_arm64.cc`, `mt_log.cc`).

### Key design points

- **Per-endpoint lifecycle**: Channels have independent write/read endpoint
  refcounts. Either end can be closed independently, and endpoint death is
  observable via `alt`/`prialt`.
- **M:N threading**: Microthreads are multiplexed across OS threads via a
  work-stealing scheduler. `init_runtime(n)` enables multi-threaded mode;
  defaults to single-threaded cooperative scheduling.
- **Type-erased compilation firewall**: Templates in the header dispatch
  through `csp::internal` (opaque `WriterRef`/`ReaderRef`/`ChanOp` types
  with `void*` pointers), keeping complex channel/scheduler logic in `.cc`
  files.
- **chan_op RAII**: Channel operations return `chan_op<T>` objects whose
  destructors call `prialt`, making `w << val;` block as a statement.
  Two-phase protocol: `prialt_begin` finds a match with locks held, typed
  transfer happens inline, then `alt_end` unlocks and schedules.

## Tests

doctest (vendored in `third_party/doctest/`). Test files in `test/`
with `.test.cc` extension. 281 tests.

## Dependencies

- **Boost.Context** (vendored as git submodule in `third_party/boost-context/`;
  only the fcontext assembly files are compiled)
- **doctest** (vendored, header-only)

### Optional tools

- **clang-tidy**: Used by `make iwyu` (include cleaner). Ships with Xcode
  Command Line Tools; standalone: `brew install llvm`.
