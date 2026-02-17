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
- **M:N threading** — microthreads are multiplexed across OS threads via a
  work-stealing scheduler. Opt-in with `init_runtime(n)`; defaults to
  single-threaded cooperative scheduling.
- **Timers** — `sleep`, `after` (one-shot), `tick` (periodic). All timers are
  channels, composable with `alt`/`prialt` for timeout patterns.
- **Stream combinators** — composable channel transformers: `buffer`, `map`,
  `where`, `tee`, `fanout`, `chain`, `quantize`, `latch`, `killswitch`,
  `enumerate`, `count`, `sink`, `blackhole`, `deaf`, `mute`, `rpc`.

## Quick start

```cpp
#include <csp/csp.h>
#include <iostream>

int main() {
    using namespace csp;

    chan<int> ch;

    // Producer — move the writer into the spawned microthread
    spawn([w = std::move(ch.w)] {
        for (int i = 0; i < 10; ++i)
            w << i;
    });

    // Consumer — read until the writer dies
    for (int n; ch.r >> n;)
        std::cout << n << "\n";
}
```

## Channel API

`chan<T>` creates a channel with public `w` (writer) and `r` (reader) members.
Endpoints are **move-only** — forgetting to move is a compile error, not a
silent deadlock. Use `.copy()` when you need shared ownership.

| Expression | Meaning |
|---|---|
| `chan<T> ch` | Create a channel with `ch.w` and `ch.r` |
| `auto [w, r] = chan<T>{}` | Structured binding for convenience |
| `std::move(ch.w)` | Move writer into a lambda/function |
| `ch.w.copy()` | Explicit shared ownership (addref) |
| `w << val` | Blocking send (returns false if reader is dead) |
| `r >> val` | Blocking receive (returns false if writer is dead) |
| `r.read()` | Blocking receive, returns value (throws if dead) |
| `~w` | Wait for writer death (in alt/prialt) |
| `~r` | Wait for reader death (in alt/prialt) |

## Multiplexing

```cpp
// alt — fair (random) selection among ready channels
int n;
switch (alt(r1 >> n, r2 >> n, ~w)) {
case 1:  /* r1 ready */  break;
case 2:  /* r2 ready */  break;
case -3: /* w died */    break;
}

// prialt — priority selection (first match wins)
prialt(r1 >> n, r2 >> n);
```

Positive results indicate data operations; negative results indicate endpoint
death (e.g., `-2` means the second channel's endpoint died).

## Timers

```cpp
#include <csp/timer.h>
using namespace std::chrono_literals;

csp::sleep(100ms);                  // block for 100ms

auto timeout = csp::after(5s);      // one-shot: fires once after 5s
auto heartbeat = csp::tick(100ms);  // periodic: fires every 100ms

// Compose with alt for timeout patterns
int n;
switch (alt(r >> n, timeout >> poke)) {
case 1:  /* data arrived */  break;
case -2: /* timed out */     break;
}
```

## M:N Threading

```cpp
csp::init_runtime(4);  // 4 OS threads (default = 0 = auto-detect)

// ... spawn microthreads as usual ...
// They are automatically distributed across OS threads
// with work stealing for load balancing.

csp::schedule();           // run until all microthreads complete
csp::shutdown_runtime();   // clean up worker threads
```

## Spawning patterns

```cpp
// Raw spawn — returns exception reader
auto ex = spawn([&]{ /* ... */ });

// Producer — returns a reader
auto r = spawn_producer<int>([](writer<int> w) {
    for (int i = 0; i < 10; ++i) w << i;
});

// Consumer — returns a writer
auto w = spawn_consumer<int>([](reader<int> r) {
    for (int n; r >> n;) process(n);
});

// Filter — returns a chan<T> with both endpoints
auto ch = spawn_filter<int>([](reader<int> r, writer<int> w) {
    for (int n; r >> n;) w << (n * 2);
});
```

## Stream combinators

| Combinator | Description |
|---|---|
| `buffer(r, w, n)` | Bounded/unbounded FIFO buffer |
| `map(r, w, f)` | Transform each element with `f` |
| `where(r, w, pred)` | Filter elements by predicate |
| `tee(r, w, side)` | Duplicate stream to a side channel |
| `fanout(new_out, new_in)` | Broadcast to dynamically added subscribers |
| `chain(readers, w)` | Concatenate multiple readers sequentially |
| `latch(r, w)` | Hold and repeat the last value |
| `killswitch(r, w, keepalive)` | Terminate when keepalive signal dies |
| `quantize(src, quanta, sink)` | Batch values into quanta |
| `count(w, start, stop, step)` | Generate integer sequences |
| `enumerate(collection, w)` | Stream elements from a collection |
| `sink(r, f)` | Consume with side-effect function |
| `blackhole(r)` | Consume and discard all messages |
| `rpc_client` / `rpc_server` | Request-response over channel pairs |

All combinators have `spawn_*` variants that return connected endpoints.

## Examples

See the [`examples/`](examples/) directory for complete programs:

- **fibonacci** — simple Fibonacci generation
- **prime_sieve** — concurrent prime sieve
- **daisy_chain** — chain of microthreads
- **pipeline** — stream processing pipeline
- **merge_sort** — concurrent merge sort
- **fan_out_fan_in** — fanout/fanin pattern
- **chat_room** — dynamic pub/sub with channel-of-channels
- **dining_philosophers** — classic concurrency problem via channels
- **timeout_patterns** — timer composition with alt
- **rate_limiter** — rate limiting with timers
- **latch_config** — configuration distribution
- **rpc_service** — request-response pattern

## Building

Requires Boost.Context and a C++17 compiler.

```bash
make        # build and run tests
make build  # compile only
make clean  # remove build artifacts
```

## Dependencies

- **Boost.Context** (linked library) — coroutine context switching
- **doctest** (vendored in `third_party/`) — test framework

## Documentation

- [`docs/overview.md`](docs/overview.md) — user guide with examples
- [`docs/architecture.md`](docs/architecture.md) — internal design and
  implementation details

## License

Apache License 2.0 — see [LICENSE](LICENSE).
