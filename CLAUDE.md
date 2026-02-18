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

### Key modules

- **include/csp/csp.h** — Main public header: `spawn`, `schedule`,
  `yield`, `chan`, `writer`, `reader`, `alt`/`prialt`, `chan_op` RAII class,
  and the `csp::internal` type-erased API.
- **include/csp/internal/** — Internal headers (csp_internal.h for
  Microthread struct, runtime.h, processor.h, stack_pool.h, hamt.h,
  reactor.h, blocking_pool.h) and vendored utilities.
- **include/csp/timer.h** — Timer primitives: `sleep`, `sleep_until`,
  `after` (one-shot), `tick` (periodic). Timers are channels; composable
  with `alt`/`prialt` for timeout patterns.
- **include/csp/io.h** — Non-blocking I/O: `read`/`write`/`accept`/`connect`
  (fd operations via kqueue reactor), `resolve` (DNS via blocking thread pool).
- **include/csp/dynamic.h** — `csp::dynamic<T>` dynamic-scoped variables
  via persistent HAMT, with `context`/`context_scope` for snapshot/restore.
- **include/csp/signal.h** — Unix signal channels via self-pipe trick.
- **include/csp/blocking.h** — Blocking thread pool for offloading
  OS-blocking calls from microthreads.
- **include/csp/part/** — 50+ header-only stream combinators in
  `namespace csp::part`. Three wrapper types (`filter`, `producer`,
  `consumer`) with `operator|` composition. Key combinators: map, where,
  scan, flat_map, batch, window, slide, merge, zip, unzip, round_robin,
  interleave, partition, group_by, share, debounce, throttle, gate,
  metrics, reduce, and more.
- **include/csp/part/part.h** — Combinator infrastructure: `filter`,
  `producer`, `consumer` wrapper types and `operator|` composition
  (8 overloads for all pairwise combinations including concrete endpoints).
- **src/** — Implementation files for microthread scheduler (`csp.cc`),
  channels (`channel.cc`), M:N runtime (`runtime.cpp`), globals
  (`csp_globals.cpp`), logging (`mt_log.cc`), I/O reactor (`reactor.cc`),
  blocking pool (`blocking_pool.cc`), signal handling (`signal.cc`),
  stack pool (`stack_pool.cc`), HAMT (`hamt.cc`), stack analysis
  (`stack_analysis_arm64.cc`).

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
