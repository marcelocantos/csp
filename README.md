# CSP

A C++ microthreading library with typed, synchronous channels inspired by
[Communicating Sequential Processes](https://en.wikipedia.org/wiki/Communicating_sequential_processes).

## Features

- **Stackful coroutines** — lightweight microthreads (32 KB stacks) via
  [Boost.Context](https://www.boost.org/doc/libs/release/libs/context/).
- **Typed synchronous channels** — unbuffered, blocking send/receive with
  compile-time type safety.
- **Per-endpoint lifecycle** — channels can be closed from either end. Endpoint
  death is observable via `alt`/`prialt`, enabling communication topologies that
  are difficult to express with conventional close-the-whole-channel semantics.
- **Alt/prialt multiplexing** — `alt` shuffles for fairness, `prialt` scans in
  priority order. Both support waiting on sends, receives, and endpoint death.
- **Stream combinators** — composable channel transformers: `buffer`, `map`,
  `where`, `tee`, `fanout`, `chain`, `quantize`, `latch`, `killswitch`,
  `enumerate`, `count`, `sink`, `blackhole`, `deaf`, `mute`, `rpc`.

## Quick start

```cpp
#include <csp/microthread.h>
#include <csp/buffer.h>
#include <iostream>

int main() {
    using namespace csp;

    channel<int> ch;

    // Producer
    spawn([w = +ch] {
        for (int i = 0; i < 10; ++i)
            w << i;
    });

    // Consumer
    spawn([r = -ch] {
        for (int n; r >> n;)
            std::cout << n << "\n";
    });

    ch.release();
    schedule();
}
```

## Channel conventions

| Expression | Meaning |
|---|---|
| `+ch` | Get a writer reference |
| `-ch` | Get a reader reference |
| `++ch` | Move the writer out |
| `--ch` | Move the reader out |
| `w << val` | Blocking send (returns false if reader is dead) |
| `r >> val` | Blocking receive (returns false if writer is dead) |
| `~w` | Wait for writer death |
| `~r` | Wait for reader death |

## Building

Requires Boost.Context and a C++17 compiler.

```bash
make        # build and run tests
make build  # compile only
make clean  # remove build artifacts
```

## Dependencies

- **Boost.Context** (linked library) — coroutine context switching
- **Google Test/Mock** (vendored in `third_party/`) — test framework

## License

Apache License 2.0 — see [LICENSE](LICENSE).
