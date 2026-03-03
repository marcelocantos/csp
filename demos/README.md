# CSP Demos

20 bite-sized programs that showcase what changes when you swap threads +
locks for imps + channels. Each demo targets **one idea**, fits in a single
file, and opens with a header comment contrasting the CSP approach with
traditional `std` C++ threading.

## Build

```bash
# From the repo root — generate the distribution files first:
make dist

# Then build and run the demos:
cd demos
make        # compile all demos
make run    # compile + run all demos
make clean  # remove build artifacts
```

The demos build against the three files in `dist/` (`csp.h`, `csp.cpp`,
`csp_globals.cpp`) — exactly as an end user would.

## Demos

### Theme 1 — Look Ma, No Locks

| # | File | What it shows |
|---|------|---------------|
| 01 | `01_ping_pong.cpp` | Two imps, two channels, bouncing a counter. The simplest possible concurrent program — no mutex, no shared state, just messages. |
| 02 | `02_no_mutex.cpp` | Five imps send increments to a server imp that owns the counter. The channel serialises access — no `std::mutex` needed. |
| 03 | `03_no_condvar.cpp` | Bounded producer/consumer via `chan<int>(4)`. Backpressure is built into the channel — no condition variables, no spurious wakeup handling. |
| 04 | `04_race_free_cache.cpp` | Concurrent key-value store. Clients send request structs with a reply channel; the server owns the map exclusively. No `shared_mutex`, no torn reads. |

### Theme 2 — Select Changes Everything

| # | File | What it shows |
|---|------|---------------|
| 05 | `05_select.cpp` | `alt()` multiplexes three data sources fairly, detecting when each dies via `~reader` vultures. In std C++ there's no `select()` for threads. |
| 06 | `06_timeout.cpp` | `prialt(r >> v, after(100ms) >> nullptr)` — timeout any channel operation in one line. Shows read timeout, write timeout, and fast-path data. |
| 07 | `07_heartbeat.cpp` | Dead peer detection via channel death. When an imp exits, `~reader` in prialt fires immediately — no heartbeat protocol needed. |
| 08 | `08_first_response.cpp` | Race 4 backends, take the first result. Drop the reader — losers' writes silently fail and they exit. No cancellation tokens. |

### Theme 3 — Primitives You Didn't Know You Wanted

| # | File | What it shows |
|---|------|---------------|
| 09 | `09_fan_out.cpp` | 4 workers share one input channel and one output channel. Workers exit when input closes; output closes when all workers finish. Zero coordination code. |
| 10 | `10_pipeline.cpp` | Declarative stream processing: `enumerate | map | where | batch`. Each stage runs concurrently with automatic backpressure. |
| 11 | `11_generator.cpp` | Infinite lazy sequences via channels. Drop the reader to cancel — no coroutine handle bookkeeping, no custom allocators. |
| 12 | `12_ten_thousand.cpp` | Spawn 10,000 imps, each doing a small task. Demand-paged stacks cost ~4 KB each — try this with `std::thread`. |

### Theme 4 — Problems That Just Disappear

| # | File | What it shows |
|---|------|---------------|
| 13 | `13_backpressure.cpp` | Fast producer, slow consumer. Synchronous channels throttle the producer automatically — no ringbuffer overflow, no dropped messages. |
| 14 | `14_graceful_shutdown.cpp` | Close one writer → the downstream pipeline drains and exits cleanly. No `stop_source`, no "please stop" protocol. |
| 15 | `15_cancellation.cpp` | `cancellation(200ms)` scope cancels a tree of worker imps. `done()` in prialt fires when the deadline expires — inherited via dynamic scoping, no token plumbing. |

### Theme 5 — Production Patterns

| # | File | What it shows |
|---|------|---------------|
| 16 | `16_live_tap.cpp` | `tap()` adds a side observer to a live channel without modifying producer or consumer code. |
| 17 | `17_supervision.cpp` | Erlang-style auto-restart: `on_exit(restart_policy{})` + `supervised()`. Worker retries automatically with configurable limits. |
| 18 | `18_broadcast.cpp` | `share()` multicasts one producer to N subscribers. Each subscriber gets its own channel. |
| 19 | `19_fake_time.cpp` | `fake_clock` replaces wall time via dynamic scoping. An hour-long sleep completes in ~60 microseconds. Essential for CI. |
| 20 | `20_dining_deadlock_free.cpp` | Classic dining philosophers — deadlock-free via channel synchronisation. Each fork is a server imp; no lock ordering needed. |
