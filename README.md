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
- **Stream combinators** — 50+ composable channel transformers in
  `namespace csp::part`, with `operator|` composition for building pipelines.
- **I/O and signals** — non-blocking file descriptor reads, DNS resolution via
  a blocking thread pool, and Unix signal channels via the I/O reactor.

## Quick start

```cpp
#include <csp/csp.h>
#include <iostream>

int main() {
    using namespace csp;

    spawn([]{
        chan<int> ch;

        // Producer — move the writer into the spawned microthread
        spawn([w = std::move(ch.w)] {
            for (int i = 0; i < 10; ++i)
                w << i;
        });

        // Consumer — read until the writer dies
        for (int n; ch.r >> n;)
            std::cout << n << "\n";
    });

    schedule();
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

## I/O and Signals

```cpp
#include <csp/io.h>
#include <csp/part/io.h>
#include <csp/signal.h>

// Non-blocking file descriptor read (requires init_runtime)
auto bytes = csp::part::byte_reader(fd).spawn();  // reader<vector<uint8_t>>

// Non-blocking DNS resolution (runs on blocking thread pool)
auto result = csp::io::resolve("example.com", "80", &hints);

// Unix signal channels (requires init_runtime)
auto sigs = csp::signal::notify({SIGINT, SIGTERM});  // reader<int>
```

## M:N Threading

```cpp
csp::init_runtime(4);  // 4 OS threads (default = single-threaded)

// ... spawn microthreads as usual ...
// They are automatically distributed across OS threads
// with work stealing for load balancing.

csp::schedule();          // run until all microthreads complete
csp::shutdown_runtime();  // stop workers, join threads
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

All combinators live in `namespace csp::part` and are header-only. Most return
composable wrapper types (`filter`, `producer`, `consumer`) that can be
connected with `operator|`.

```cpp
using namespace csp::part;
// Pipeline: generate → transform → consume
auto pipeline = count(1, 100) | map<int, int>([](int n){ return n * n; })
                               | where<int>([](int n){ return n % 2 == 0; });
auto r = pipeline.spawn();  // reader<int>
```

### Sources

| Combinator | Description |
|---|---|
| `count(start, stop, step)` | Generate integer sequences |
| `enumerate(collection)` | Stream elements from a collection |

### Transforms

| Combinator | Description |
|---|---|
| `map(f)` | Transform each element with `f` |
| `where(pred)` | Filter elements by predicate |
| `scan(init, f)` | Running fold — emit accumulator after each input |
| `flat_map(f)` | Map to sub-streams, merge non-deterministically |
| `flatten` | Flatten stream of containers into elements |
| `batch(n)` | Collect n elements into vectors |
| `window(n)` | Sliding window as vector snapshots |
| `slide(pred)` / `slide_fixed(n)` | Sliding window with enter/exit events |
| `nwise(n)` / `pairwise` | Sliding n-tuples / consecutive pairs |
| `distinct` / `unique(n)` | Suppress consecutive / all-time duplicates |
| `take_while(pred)` / `skip_while(pred)` | Predicate-based take/drop |
| `first(n)` / `last(n)` | First/last n elements |
| `skip_first(n)` / `skip_last(n)` | Drop first/last n elements |
| `stride(n)` | Every nth element |
| `default_if_empty(val)` | Emit default if input is empty |
| `delay(d)` | Delay each value by duration d |
| `debounce(d)` | Emit after quiet period elapses |
| `throttle(n, interval)` | Rate-limit to n values per interval |
| `sample(trigger)` | Emit latest value on each trigger |
| `timeout(d)` | Close if no value within duration d |

### Fan-out / Fan-in

| Combinator | Description |
|---|---|
| `tee(side)` | Duplicate stream to a side channel |
| `fanout` | Broadcast to dynamically added subscribers |
| `merge(readers...)` | Non-deterministic merge of N readers |
| `zip(readers...)` | Combine N readers into tuples |
| `unzip` | Split tuple stream into separate readers |
| `round_robin(n)` | Distribute across N outputs deterministically |
| `interleave(readers...)` | Strict round-robin merge of N inputs |
| `partition(n, f)` | Route to one of N outputs by classifier |
| `group_by(f)` | Dynamic partitioning by key |
| `share` | Pub/sub with per-subscriber backpressure |
| `first_wins(readers...)` | Read from whichever source responds first |

### Lifecycle / Control

| Combinator | Description |
|---|---|
| `buffer(n)` | Bounded/unbounded FIFO buffer |
| `latch` | Hold and repeat the last value |
| `killswitch(keepalive)` | Terminate when keepalive dies |
| `gate(control)` | Pause/resume via control channel |
| `reduce(init, f)` | Fold to single value |
| `join(readers...)` | Block until all channels close |
| `metrics` | Pull-based throughput stats (count, elapsed) |
| `sink(f)` | Consume with side-effect function |
| `blackhole` | Consume and discard all values |
| `deaf` / `mute` | Dead-end reader / writer |
| `quantize(quanta)` | Batch values into quanta |
| `rpc_client` / `rpc_server` | Request-response over channel pairs |

### Controlled timer

```cpp
#include <csp/part/timer.h>
chan<clock::duration> ctl;
auto t = csp::part::timer(std::move(ctl.r));  // or clock::time_point
```

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

### Guide
- [Getting Started](docs/guide/01-getting-started.md)
- [Channels](docs/guide/02-channels.md)
- [Multiplexing](docs/guide/03-multiplexing.md)
- [Timers](docs/guide/04-timers.md)
- [Combinators & Parts](docs/guide/05-combinators.md)
- [I/O](docs/guide/06-io.md)
- [Blocking Calls](docs/guide/07-blocking.md)
- [Signals](docs/guide/08-signals.md)
- [Concurrency & M:N Runtime](docs/guide/09-concurrency.md)
- [Error Handling](docs/guide/10-error-handling.md)
- [Common Pitfalls](docs/guide/11-pitfalls.md)

### Reference
- [Parts Catalog](docs/reference/parts.md) — all 50+ stream combinators
- [Architecture](docs/architecture.md) — internal design

## License

Apache License 2.0 — see [LICENSE](LICENSE).
