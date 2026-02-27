# Getting Started

CSP is a C++ imp-based concurrency library with typed, synchronous channels inspired
by [Communicating Sequential Processes][wp-csp]. Lightweight imps
(32 KB stacks) communicate exclusively through blocking channels -- no shared
memory, no locks.

[wp-csp]: https://en.wikipedia.org/wiki/Communicating_sequential_processes

## Prerequisites

- **C++20** compiler (Clang with libc++ recommended)

## Integration

Copy the three distribution files (`csp.h`, `csp.cpp`, `csp_globals.cpp`) into
your project and compile both `.cpp` files as separate translation units. There
are no external dependencies.

## Your first program

The program below spawns a producer imp that sends the numbers 0--9
over a channel, and a main imp that reads and prints them.

```cpp
#include "csp.h"
#include <iostream>

int main() {
    using namespace csp;

    // Wrap all work in an imp, then run the scheduler.
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

    schedule();   // drive imps to completion
}
```

### What is happening

1. `chan<int>{}` creates an unbuffered, synchronous channel with two endpoints:
   a `writer<int>` (`ch.w`) and a `reader<int>` (`ch.r`).
2. `spawn` launches a new imp. The writer is *moved* into the lambda
   -- endpoints are move-only, so forgetting `std::move` is a compile error,
   not a silent deadlock.
3. `w << i` blocks the producer until the consumer is ready to receive.
   `r >> n` (or the range-for equivalent) blocks the consumer until the
   producer sends.
4. When the producer lambda returns, the writer is destroyed. The consumer's
   range-for loop sees the closed channel and exits.
5. `schedule()` runs the cooperative scheduler until all imps have
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
  (`map`, `where`, `scan`, ...)
- The [`examples/`](../../examples/) directory has complete programs covering
  Fibonacci generation, prime sieves, pipelines, fan-out/fan-in, chat rooms,
  and more.
