# CSP: A C++ Microthreading Library

CSP is a C++ library for concurrent programming based on Communicating
Sequential Processes. It provides lightweight cooperative microthreads
(coroutines) that communicate through typed, synchronous channels, with an
M:N scheduler that maps microthreads across multiple OS threads.

## Core Concepts

**Microthreads** are lightweight, cooperatively scheduled threads of execution.
Each microthread has a 32 KB stack and context-switches via Boost.Context.
Microthreads yield control at channel operations, timer sleeps, or explicit
`csp_yield()` calls. A program can sustain millions of microthreads.

**Channels** are typed, synchronous, unbuffered conduits between microthreads.
A `channel<T>` has a `writer<T>` endpoint and a `reader<T>` endpoint.
Sending blocks until a receiver is ready; receiving blocks until a sender is
ready. Endpoints are reference-counted and independently closeable---a reader
can detect when all writers are gone, and vice versa.

**Alt/Prialt** provide multi-way select. `alt()` blocks until one of several
channel operations is ready, choosing randomly among simultaneously ready
alternatives for fairness. `prialt()` chooses by index priority. Both can
wait on data readiness or endpoint closure.

## Quick Start

```cpp
#include <csp/microthread.h>
#include <csp/timer.h>
#include <iostream>

int main() {
    // Create a channel carrying integers.
    csp::channel<int> ch;

    // Spawn a producer that sends 1..10 then closes.
    csp::spawn([w = ++ch] {
        for (int i = 1; i <= 10; ++i)
            w << i;
    });

    // Spawn a consumer that reads until the writer closes.
    csp::spawn([r = --ch] {
        for (int v : r)
            std::cout << v << "\n";
    });

    csp::schedule();
}
```

### Endpoint Operators

Channels expose their endpoints via operator overloads on `channel<T>`:

| Expression | Meaning                           |
|------------|-----------------------------------|
| `++ch`     | Move the writer out (single use)  |
| `--ch`     | Move the reader out (single use)  |
| `+ch`      | Borrow a writer reference         |
| `-ch`      | Borrow a reader reference         |

Sending and receiving return `action` objects whose destructors call `prialt`,
so `w << value;` and `r >> variable;` block as statements:

```cpp
w << 42;          // blocks until a reader accepts
int v; r >> v;    // blocks until a writer sends
```

The boolean result indicates success (`true`) vs. closed endpoint (`false`):

```cpp
if (!(w << 42)) { /* reader is gone */ }
int v;
while (r >> v) { /* process v */ }
```

## Multi-Way Select

`alt` and `prialt` select from multiple pending channel operations:

```cpp
csp::channel<int> data;
auto timeout = csp::after(std::chrono::seconds(5));

int v;
switch (csp::alt(data >> v, timeout >> csp::poke)) {
case 1: std::cout << "received " << v << "\n"; break;
case 2: std::cout << "timed out\n"; break;
}
```

Wait for endpoint closure with the `~` operator:

```cpp
csp::prialt(~writer_endpoint, reader >> value);
```

## Spawning Patterns

`spawn` is the general-purpose launcher. Specialised variants create
microthreads pre-wired to channel endpoints:

```cpp
// General spawn --- returns a reader for the exception channel.
auto ex = csp::spawn([&] { /* ... */ });
csp::join(ex);  // rethrows if the microthread threw

// Producer: receives a writer, caller gets matching reader.
auto r = csp::spawn_producer<int>([](csp::writer<int> w) {
    for (int i = 0; i < 100; ++i) w << i;
});

// Consumer: receives a reader, caller gets matching writer.
auto w = csp::spawn_consumer<int>([](csp::reader<int> r) {
    for (int v : r) process(v);
});

// Filter: receives both, caller gets the inverse channel.
auto ch = csp::spawn_filter<int>([](csp::reader<int> r, csp::writer<int> w) {
    for (int v : r) w << (v * 2);
});
```

## Timers

Timers are channels, composable with `alt`/`prialt`:

```cpp
#include <csp/timer.h>
using namespace std::chrono_literals;

csp::sleep(100ms);                     // block for duration
csp::sleep_until(csp::clock::now() + 1s); // block until deadline

auto timeout = csp::after(5s);         // one-shot: fires once
auto ticker  = csp::tick(100ms);       // periodic: delivers time_points
```

## Stream Combinators

Header-only combinators compose microthreads into pipelines. Each combinator
spawns a microthread internally:

| Combinator   | Purpose                                        |
|--------------|------------------------------------------------|
| `buffer`     | Bounded buffer between producer and consumer   |
| `map`        | Transform each element                         |
| `where`      | Filter by predicate                            |
| `tee`        | Duplicate stream to a side channel             |
| `fanout`     | Broadcast to dynamic subscribers               |
| `chain`      | Concatenate multiple input readers             |
| `latch`      | Hold and repeat the last value                 |
| `killswitch` | Terminate when a keepalive signal dies         |
| `quantize`   | Batch values into quanta                       |
| `enumerate`  | Stream elements from a collection              |
| `count`      | Generate integer sequences                     |
| `sink`       | Consume with side-effect function              |
| `blackhole`  | Consume and discard                            |
| `rpc`        | Request-response over channels                 |

Example pipeline:

```cpp
auto r = csp::spawn_count<int>(0, 100, 1);          // 0..99
r = csp::spawn_where<int>(std::move(r), [](int v) { return v % 2 == 0; });
r = csp::spawn_map<int, std::string>(std::move(r), [](int v) {
    return std::to_string(v);
});
auto w = csp::spawn_sink<std::string>([](std::string const& s) {
    std::cout << s << "\n";
});
```

## M:N Runtime

By default, CSP runs single-threaded. Call `init_runtime` to enable
multi-threaded execution:

```cpp
csp::init_runtime(4);   // 4 OS threads (0 = auto-detect)
// ... spawn microthreads ...
csp::schedule();         // blocks until all microthreads complete
csp::shutdown_runtime();
```

In M:N mode, microthreads are distributed across OS worker threads. The
runtime provides:

- **Global run queue** for load balancing across processors.
- **Work stealing** so idle workers take work from busy ones.
- **Per-processor timer heaps** for efficient timer management.
- **Worker parking** with condition variables to avoid busy-waiting.

All channel operations are safe across OS threads. The library uses lock
ordering, atomic CAS for wakeup coordination, and a suspension protocol
to prevent races during context switches.

## Build

```bash
make            # build and run all tests
make build      # compile only
make bench      # build and run benchmarks
make clean      # remove build artifacts
make SANITIZE=thread              # ThreadSanitizer build
make SANITIZE=address,undefined   # ASan + UBSan build
```

Requirements: Clang with C++17 and libc++, Boost.Context.

## Project Layout

```
include/csp/
    microthread.h           Public API: spawn, channels, alt/prialt, action
    timer.h                 Timer primitives: sleep, after, tick
    ringbuffer.h            Internal ring buffer utility
    fcontext.h              Boost.Context type aliases
    buffer.h map.h ...      Stream combinator headers (header-only)
    internal/
        microthread_internal.h   Microthread struct, scheduling primitives
        runtime.h                M:N runtime coordinator
        processor.h              Per-processor state
        mt_log.h                 Debug logging infrastructure

src/
    microthread.cc           Context switching, run queue, spawn
    channel.cc               Channel and alt/prialt implementation
    runtime.cpp              M:N worker loop, work stealing, parking
    microthread_globals.cpp  Thread-local state, runtime init/shutdown

test/
    *.test.cc                doctest-based test suite
```
