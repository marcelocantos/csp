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

All code lives in `namespace csp`. Key modules:

- **include/csp/microthread.h** — Main public header: `spawn`, `schedule`,
  `channel`, `writer`, `reader`, `alt`/`prialt`, `action` RAII class.
- **include/csp/internal/** — Internal headers (microthread_internal.h,
  mt_log.h) and vendored utilities (on_scope_exit.h, function.h).
- **Stream combinators** — Header-only: buffer, map, where, tee, fanout,
  chain, quantize, latch, killswitch, enumerate, count, sink, blackhole,
  deaf, mute, rpc.
- **src/** — Implementation files for microthread scheduler, channels,
  ring buffer, logging.

### Key design points

- **Per-endpoint lifecycle**: Channels have independent write/read endpoint
  refcounts. Either end can be closed independently, and endpoint death is
  observable via `alt`/`prialt`.
- **Cooperative scheduling**: Microthreads run on a circular doubly-linked
  busy queue (`g_self`, `g_busy`). Context switching via Boost.Context.
- **Action RAII**: Channel operations return `action` objects whose
  destructors call `prialt`, making `w << val;` block as a statement.

## Tests

doctest (vendored in `third_party/doctest/`). Test files in `test/`
with `.test.cc` extension.

## Dependencies

- **Boost.Context** (linked: `-lboost_context`)
- **doctest** (vendored, header-only)

## C-linkage API prefix

All C-linkage symbols use the `csp_` prefix (e.g., `csp_spawn`, `csp_run`,
`csp_yield`, `csp_alt`, `csp_prialt`).
