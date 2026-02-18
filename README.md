# CSP

A C++ microthreading library with typed, synchronous channels inspired by
[Communicating Sequential Processes](https://en.wikipedia.org/wiki/Communicating_sequential_processes).

## Features

- **Lightweight userspace threads** with M:N scheduling and work stealing
- **Typed synchronous channels** — unbuffered, blocking send/receive
- **Per-endpoint lifecycle** — either end can close independently; death is
  observable via `alt`/`prialt`
- **Alt/prialt multiplexing** — select across sends, receives, and endpoint death
- **Timers** — `sleep`, `after`, `tick`, all composable with `alt`
- **50+ stream combinators** — `map`, `where`, `scan`, `merge`, `zip`, and more,
  with `operator|` composition
- **Non-blocking I/O and Unix signals** via kqueue reactor
- **Dynamic scoping** — microthread-scoped variables with copy-on-write isolation

## Quick start

CSP is distributed as three files. Copy them into your project from `dist/`:

| File | Notes |
|---|---|
| `csp.h` | Single header — all public API |
| `csp.cpp` | Implementation + fcontext assembly |
| `csp_globals.cpp` | Thread-local state — **must be a separate translation unit** ([why](docs/tls-caching-bug.md)) |

No external dependencies. Compile both `.cpp` files alongside your code:

```bash
c++ -std=c++17 -O2 -c csp.cpp -o csp.o
c++ -std=c++17 -O2 -c csp_globals.cpp -o csp_globals.o
c++ -std=c++17 -O2 -c my_app.cpp -o my_app.o
c++ my_app.o csp.o csp_globals.o -o my_app
```

### Hello, channels

```cpp
#include "csp.h"
#include <iostream>

int main() {
    using namespace csp;

    spawn([] {
        chan<int> ch;

        // Producer — move the writer into the spawned microthread.
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
microthreads to completion.

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
  [Dynamic Scoping](docs/guide/12-dynamic-scoping.md)
- **Reference** — [Parts Catalog](docs/reference/parts.md) (50+ stream combinators)
- **Architecture** — [Internal Design](docs/architecture.md)
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

## License

Apache License 2.0 — see [LICENSE](LICENSE).
