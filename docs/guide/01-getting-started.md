# Getting Started

CSP is a C++ microthreading library with typed, synchronous channels inspired
by [Communicating Sequential Processes][wp-csp]. Lightweight microthreads
(32 KB stacks) communicate exclusively through blocking channels -- no shared
memory, no locks.

[wp-csp]: https://en.wikipedia.org/wiki/Communicating_sequential_processes

## Prerequisites

- **C++17** compiler (Clang with libc++ recommended)
- **Boost.Context** (linked library for coroutine context switching)

## Building

```bash
make        # build and run all tests
make build  # compile only
make clean  # remove build artifacts
```

## Your first program

The program below spawns a producer microthread that sends the numbers 0--9
over a channel, and a main microthread that reads and prints them.

```cpp
#include "csp.h"
#include <iostream>

int main() {
    using namespace csp;

    // Wrap all work in a microthread, then run the scheduler.
    spawn([] {
        // Create a channel. ch.w is the writer endpoint, ch.r is the reader.
        chan<int> ch;

        // Spawn a producer. Move the writer in so the channel closes
        // automatically when the lambda returns.
        spawn([w = std::move(ch.w)] {
            for (int i = 0; i < 10; ++i)
                w << i;
        });

        // Read until the channel closes (writer destroyed).
        for (int n : ch.r)
            std::cout << n << "\n";
    });

    schedule();   // drive microthreads to completion
}
```

### What is happening

1. `chan<int>` creates an unbuffered, synchronous channel with two endpoints:
   a `writer<int>` (`ch.w`) and a `reader<int>` (`ch.r`).
2. `spawn` launches a new microthread. The writer is *moved* into the lambda
   -- endpoints are move-only, so forgetting `std::move` is a compile error,
   not a silent deadlock.
3. `w << i` blocks the producer until the consumer is ready to receive.
   `r >> n` (or the range-for equivalent) blocks the consumer until the
   producer sends.
4. When the producer lambda returns, the writer is destroyed. The consumer's
   range-for loop sees the closed channel and exits.
5. `schedule()` runs the cooperative scheduler until all microthreads have
   completed.

### Structured bindings

If you prefer, you can destructure the channel directly:

```cpp
auto [w, r] = chan<int>{};
spawn([w = std::move(w)] { /* ... */ });
for (int n : r) { /* ... */ }
```

## Next steps

- [`02-channels.md`](02-channels.md) -- channel semantics, endpoint
  lifecycle, and shared ownership
- [`03-multiplexing.md`](03-multiplexing.md) -- `alt` and `prialt` for
  selecting across multiple channels
- [`04-timers.md`](04-timers.md) -- `sleep`, `after`, `tick`, and timeout
  patterns
- [`05-combinators.md`](05-combinators.md) -- composable stream transformers
  (`map`, `where`, `scan`, `buffer`, ...)
- The [`examples/`](../../examples/) directory has complete programs covering
  Fibonacci generation, prime sieves, pipelines, fan-out/fan-in, chat rooms,
  and more.
