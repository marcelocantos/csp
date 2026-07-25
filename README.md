# CSP

A C++ imp-based concurrency library with typed, synchronous channels inspired by
[Communicating Sequential Processes](https://en.wikipedia.org/wiki/Communicating_sequential_processes).

## Features

- **Lightweight userspace threads (imps)** with M:N scheduling and work stealing
- **Typed channels** — synchronous (unbuffered) or buffered (`chan<T>(n)`)
- **Per-endpoint lifecycle** — [bidirectional lifecycle
  observability](docs/papers/15-channels-as-interfaces.md): either end can
  close independently; death is observable via `alt`/`prialt`
- **Alt/prialt multiplexing** — select across sends, receives, and endpoint death
- **Timers** — `sleep`, `after`, `tick`, all composable with `alt`
- **70+ stream combinators** — `map`, `where`, `scan`, `merge`, `zip`, and more,
  with `operator|` composition
- **Non-blocking I/O** via platform-native reactor (kqueue/epoll/WSAEventSelect)
- **TCP networking** — `net::listen` / `net::dial` with pull-based sources
- **HTTP/1.1, HTTP/2, HTTP/3, WebSocket, QUIC** — optional per-protocol drop-ins
- **Unix/Windows signals** — signal channels composable with `alt`
- **TLS** — cancel-aware TLS 1.3 via PicoTLS (`#ifdef CSP_TLS`)
- **Cooperative cancellation** — scope-based, with deadlines, composable in `alt`
- **Dynamic scoping** — inherited variables with scoped bindings and copy-on-write
  isolation
- **Request/response** — `request<Req, Resp>` type with `call()` for
  non-blocking RPC and callable writer endpoints
- **Imp-local storage** — per-imp variables (not inherited)
- **Imp exit / supervision** — restart policies, worker groups, supervised execution
- **Cross-platform** — macOS, Linux (x86_64/arm64), Windows (x86_64)

## Quick start

CSP is distributed as a small set of vendor drop-in files from `dist/`. The
**core trio** is always required; network protocols are optional extra
`.cpp` files (see [`dist/AGENTS-CSP.md`](dist/AGENTS-CSP.md) and
[per-protocol dist design](docs/design/per-protocol-dist.md)).

| File | Notes |
|---|---|
| `csp.h` | Single header — all public API |
| `csp.cpp` | Core implementation + fcontext asm |
| `csp_globals.cpp` | Thread-local state — **must be a separate translation unit** ([why](docs/tls-caching-bug.md)) |
| `csp_<proto>.cpp` | Optional: `tls`, `http`, `http2`, `ws`, `quic`, `http3` |

Core channels need no third-party libraries. Protocol drop-ins need the
matching vendored deps — fetch them with
[`scripts/vendor-deps.sh`](scripts/vendor-deps.sh). Compile the core trio
alongside your code:

```bash
c++ -std=c++20 -O2 -c csp.cpp -o csp.o
c++ -std=c++20 -O2 -c csp_globals.cpp -o csp_globals.o
c++ -std=c++20 -O2 -c my_app.cpp -o my_app.o
c++ my_app.o csp.o csp_globals.o -o my_app
```

Prefer not to compile CSP yourself? Each release also ships **pre-built static
and shared libraries** for macOS arm64 and Linux x86_64/arm64 — link
`-lcsp` (plus `-lcsp_<proto>` for any network protocols) against the published
archives. See [pre-built library artefacts](docs/design/prebuilt-libs.md) for
the per-platform link incantations and the libc++ ABI policy.

### Hello, channels

```cpp
#include "csp.h"
#include <iostream>

int main() {
    using namespace csp;

    spawn([] {
        chan<int> ch;

        // Producer — move the writer into the spawned imp.
        spawn([w = std::move(ch.w)] {
            for (int i = 0; i < 10; ++i)
                w << i;
        });

        // Consumer — read until the writer dies.
        for (int n : ch.r)
            std::cout << n << "\n";
    });

    schedule();
}
```

`chan<int>` creates an unbuffered channel with a `writer<int>` and a
`reader<int>`. Endpoints are move-only — forgetting `std::move` is a compile
error, not a silent deadlock. `w << i` blocks until the consumer is ready;
the range-for blocks until the producer sends. When the producer returns, the
writer is destroyed and the consumer's loop exits. `schedule()` drives
imps to completion.

## Documentation

- **Guide** — start here:
  [Getting Started](docs/guide/01-getting-started.md) ·
  [Channels](docs/guide/02-channels.md) ·
  [Multiplexing](docs/guide/03-multiplexing.md) ·
  [Timers](docs/guide/04-timers.md) ·
  [Combinators](docs/guide/05-combinators.md) ·
  [I/O](docs/guide/06-io.md) ·
  [Blocking Calls](docs/guide/07-blocking.md) ·
  [Signals](docs/guide/08-signals.md) ·
  [M:N Runtime](docs/guide/09-concurrency.md) ·
  [Error Handling](docs/guide/10-error-handling.md) ·
  [Pitfalls](docs/guide/11-pitfalls.md) ·
  [Dynamic Scoping](docs/guide/12-dynamic-scoping.md) ·
  [Supervision](docs/guide/13-supervision.md) ·
  [Cancellation](docs/guide/14-cancellation.md) ·
  [TLS](docs/guide/15-tls.md)
- **Reference** — [Parts Catalog](docs/reference/parts.md) (70+ stream combinators) ·
  [Transition Rules](docs/reference/transition-rules.md) (notation guide)
- **Architecture** — [Internal Design](docs/architecture.md)
- **Papers** — [The Engineering of CSP](docs/papers/) (TLS bugs,
  formal verification, zero-overhead channels, and more)
- **Examples** — [`examples/`](examples/) (Fibonacci, prime sieve, pipelines,
  fan-out/fan-in, chat room, dining philosophers, and more)

## Development

```bash
make        # build and run tests
make build  # compile only
make dist   # regenerate distribution files from source
make check  # run TLA+ model checker
make clean  # remove build artifacts
```

On arm64 (macOS/Linux), `make LIGHT_SWITCH=1` — or `-DCSP_LIGHT_SWITCH`
when compiling the dist files — opts into a minimal-save context switch
that roughly halves switch cost. The flag self-disables where it doesn't
apply (other architectures, Windows, sanitizer builds), so it is always
safe to pass.

## For coding agents

If you use an agentic coding tool, include [`dist/AGENTS-CSP.md`](dist/AGENTS-CSP.md)
in your project context for a token-efficient API reference.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
