# CLAUDE.md

## Project Overview

CSP is a C++ microthreading library with typed, synchronous channels based on
Communicating Sequential Processes. Namespace: `csp`.

## Build System

```bash
make        # build and run all tests
make build  # compile only
make clean  # remove build/
```

Build artifacts go to `build/`. Compiler: Clang, C++17, libc++, `-O2 -g`.

## Architecture

All code lives in `namespace csp`. Internal implementation details live in
`namespace csp::internal` (type-erased channel operations, scheduler
primitives) and `namespace csp::detail` (microthread struct, context
switching).

### Key modules

- **include/csp/csp.h** — Main public header: `spawn`, `schedule`,
  `yield`, `chan`, `writer`, `reader`, `alt`/`prialt`, `chan_op` RAII class,
  and the `csp::internal` type-erased API.
- **include/csp/internal/** — Internal headers (csp_internal.h for
  Microthread struct, runtime.h, processor.h) and vendored utilities.
- **include/csp/timer.h** — Timer primitives: `sleep`, `sleep_until`,
  `after` (one-shot), `tick` (periodic). Timers are channels; composable
  with `alt`/`prialt` for timeout patterns.
- **Stream combinators** — Header-only, in `namespace csp`: buffer, map,
  where, tee, fanout, chain, quantize, latch, killswitch, enumerate, count,
  sink, blackhole, deaf, mute, rpc.
- **src/** — Implementation files for microthread scheduler (`csp.cc`),
  channels (`channel.cc`), M:N runtime (`runtime.cpp`), globals
  (`csp_globals.cpp`), logging (`mt_log.cc`).

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
with `.test.cc` extension. 135 tests.

## Dependencies

- **Boost.Context** (linked: `-lboost_context`)
- **doctest** (vendored, header-only)
