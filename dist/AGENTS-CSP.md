# CSP Agent Guide

Token-efficient reference for coding agents. Covers the full API surface,
common idioms, and critical gotchas. For narrative explanations see `guide/`.

## Files

CSP is distributed as three files:

| File | Content |
|---|---|
| `csp.h` | Single header: core API, timers, I/O, signals, blocking, dynamic scoping, 50+ stream combinators |
| `csp.cpp` | Implementation source + fcontext inline assembly |
| `csp_globals.cpp` | Thread-local state (must be a separate TU) |

All user code needs only `#include "csp.h"`.

## Core Types

```cpp
namespace csp {

// Synchronous typed channel. Default T = poke_t (empty message).
template <typename T = poke_t> struct chan { writer<T> w; reader<T> r; };

// Move-only endpoints. Use .copy() for shared ownership.
template <typename T = poke_t> class writer;  // w << val  → chan_op<T>
template <typename T = poke_t> class reader;  // r >> var  → chan_op<T>

// RAII channel operation. Destructor blocks (calls prialt).
// operator bool() blocks and returns true if data transferred, false if dead.
template <typename T> class chan_op;

// Empty-message sentinel. chan<> is shorthand for chan<poke_t>.
extern struct poke_t {} poke;

// Dead-channel reader (always matches immediately as dead).
extern reader<> const skip;

// Non-blocking guard. Fires when no other op is ready.
// Returns csp::none (INT_MIN), usable as a switch case label.
//   switch (prialt(ch >> val, csp::none)) {
//     case 0:         /* read matched  */ break;
//     case csp::none: /* nothing ready */ break;
//   }
inline constexpr none_t none{};
}
```

## Lifecycle

```cpp
// Spawn an imp. Returns reader<exception_ptr> (join handle).
// f is taken BY VALUE (moved into the imp).
template <typename F> reader<std::exception_ptr> spawn(F&& f);

// Block until spawned imp finishes; rethrows its exception.
void join(reader<std::exception_ptr> const& r);

// Run the scheduler (blocks until all imps complete).
void schedule();

// M:N runtime (required for I/O, timers, signals, blocking).
void init_runtime(int num_procs = 0);  // 0 = hardware_concurrency
void shutdown_runtime();

// Cooperative yield (no-op outside an imp).
void yield();
```

## Channel Operations

```cpp
// Create a channel.
auto [w, r] = chan<int>{};

// Shorthand: create channel from endpoint reference.
reader<int> r = --w;   // w must be unattached
writer<int> w = ++r;   // r must be unattached

// Write (blocks until reader accepts or channel dies).
w << 42;                     // statement: blocks via chan_op destructor
if (w << 42) { /* sent */ }  // expression: blocks, tests success

// Read (blocks until writer sends or channel dies).
int v;
r >> v;                      // statement: blocks
if (r >> v) { /* got v */ }  // expression: blocks, tests success

// Read and return (throws csp::error if dead).
int v = r.read();

// Range-for over reader (reads until channel dies).
for (int v : r) { process(v); }

// Pipe reader directly to writer (blocks until either dies).
spawn(r.stream_to(std::move(w)));

// Shared ownership (endpoints are move-only by default).
auto w2 = w.copy();   // increments refcount
auto r2 = r.copy();

// Death watch (fires when the other endpoint is dropped).
chan_op<T> op = ~w;    // fires when all readers of w's channel die
chan_op<T> op = ~r;    // fires when all writers of r's channel die
```

## Alt / Prialt

Select over multiple channel operations. `prialt` tries in order; `alt`
randomizes. Both block until one operation completes.

```cpp
// Return value:
//   non-negative n → operation n matched (0-based)
//   complement ~n  → death event for operation n (0-based)

int v;
switch (prialt(w << 42, r >> v, ~some_reader)) {
    case 0:  /* wrote 42 */          break;
    case 1:  /* read into v */       break;
    case ~0: /* w's reader died */   break;
    case ~1: /* r's writer died */   break;
    case ~2: /* ~some_reader: writer of some_reader died */ break;
}
```

Key: `~endpoint` is a death-watch operation. When it fires, the return
value is **complemented** (`~k` for the `k`-th operation). All death
events — both explicit vultures and implicit death on data operations —
return complemented indices.

**`after()` returns non-negative**: `after(d)` sends a `time_point` then
dies, so `prialt(ch >> v, after(1s) >> nullptr)` returns `1` on timeout (data
match on the time_point value), not `~1`.

```cpp
// Vector overload (all ops must be same type T).
std::vector<chan_op<int>> ops;
ops.push_back(r1 >> v);
ops.push_back(r2 >> v);
int result = alt(ops);  // or prialt(ops)

// Non-blocking poll with none.
switch (prialt(r >> v, csp::none)) {
    case 0:         /* got v */       break;
    case csp::none: /* would block */ break;
}

// Vector overload with none.
int result = alt(ops, csp::none);  // or prialt(ops, csp::none)
```

## Spawn Helpers

```cpp
// Spawn a producer imp, return its output reader.
template <typename T, typename F>
reader<T> spawn_producer(F&& f);
// f signature: void(writer<T>)

// Spawn a consumer imp, return its input writer.
template <typename T, typename F>
writer<T> spawn_consumer(F&& f);
// f signature: void(reader<T>)

// Spawn a filter imp, return {input_writer, output_reader}.
template <typename T, typename F>
chan<T> spawn_filter(F&& f);
// f signature: void(reader<T>, writer<T>)

// Spawn a producer with exception propagation via range iterator.
template <typename T, typename F>
range<T> spawn_range(F&& f);
// for (auto& v : spawn_range<int>(f)) { ... }  // rethrows on iteration
```

## Timers

Requires `init_runtime()`.

```cpp
using clock = std::chrono::steady_clock;

void sleep(clock::duration d);
void sleep_until(clock::time_point tp);

// One-shot: reader<time_point> that fires after duration, then dies.
reader<clock::time_point> after(clock::duration d);

// Periodic: reader<clock::time_point> that fires at interval (drift-free).
reader<clock::time_point> tick(clock::duration interval);
```

Timeout idiom:
```cpp
int v;
switch (prialt(r >> v, after(100ms) >> nullptr)) {
    case 0:  handle(v);  break;
    case 1:  timeout();  break;   // after() sent time_point → non-negative match
}
```

## I/O

Requires `init_runtime()`. Uses kqueue reactor (macOS).

```cpp
namespace csp::io {
// Layer 1: suspend until fd ready.
void wait_readable(int fd);
void wait_writable(int fd);

// Layer 2: auto-retry on EAGAIN/EINTR.
ssize_t read(int fd, void* buf, size_t len);   // 0=EOF, -1=error
ssize_t write(int fd, const void* buf, size_t len); // writes ALL, -1=error
int accept(int listen_fd, sockaddr* addr, socklen_t* addrlen);
int connect(int fd, const sockaddr* addr, socklen_t addrlen); // 0=ok

// Utility.
int set_nonblock(int fd);

// DNS (runs on blocking pool).
resolve_result resolve(const std::string& host,
                       const std::string& service = {},
                       const addrinfo* hints = nullptr);
// resolve_result { int error; addrinfo_ptr info; const char* error_string(); }
}
```

Layer 3 parts:
```cpp
// byte_reader: fd → reader<vector<uint8_t>>. Owns fd, closes on exit.
auto r = csp::part::io::byte_reader(fd).spawn();          // 4096 chunks
auto r = csp::part::io::byte_reader(fd, 65536).spawn();   // custom size

// byte_writer: writer<vector<uint8_t>> → fd. Owns fd, closes on exit.
auto w = csp::part::io::byte_writer(fd).spawn();

// split_lines: byte stream → string stream (LF-delimited).
auto lr = csp::part::io::split_lines.spawn(std::move(byte_reader));

// fixed_frames: byte stream → fixed-size frames.
auto fr = csp::part::io::fixed_frames(512).spawn(std::move(byte_reader));

// Pipeline composition:
auto lr = csp::part::io::byte_reader(fd) | csp::part::io::split_lines;
auto line_reader = lr.spawn();
```

## Signals

```cpp
// Returns reader<int> emitting signal numbers. Requires init_runtime().
auto sig = csp::signal::notify({SIGINT, SIGTERM});
int s;
switch (prialt(data >> v, sig >> s)) {
    case 0: process(v);  break;
    case 1: shutdown(s);  break;
}
```

## Blocking

```cpp
// Run fn on OS thread pool; suspend calling imp until done.
template <typename Fn> auto blocking(Fn&& fn) -> invoke_result_t<Fn>;

// Example:
auto result = csp::blocking([]{ return expensive_syscall(); });
```

## Dynamic Scoping

```cpp
// Typed dynamic variable. *var reads, var = val creates a binding.
csp::dynamic<int> depth(0);

// local: RAII scoped binding (reverts when l is destroyed).
{ csp::local l{depth = *depth + 1};
  // *depth == 1 here
}  // depth restored to 0

// Multiple bindings in one local.
csp::local l{depth = 1, user = std::string("alice")};

// Bare assignment asserts in debug (catches accidental unscoped mutations).
depth = 42;  // assert failure + [[nodiscard]] warning

// context: snapshot + transfer across channels.
auto ctx = csp::context::current();
spawn([ctx] {
    csp::context_scope scope(ctx);  // install foreign context
    // *depth == 1 here
});

// Spawned imps inherit parent's context automatically.

// Imp-local (not inherited, direct write).
csp::imp_local<int> counter;
counter = 42;       // direct write, no local needed
int v = *counter;   // 42
// Child imps start with default (0), not parent's value.
```

## Parts System

Three wrapper types for composable stream stages:

| Type | Wraps | `spawn()` returns | Bound endpoint |
|---|---|---|---|
| `producer<T,F>` | `void(writer<T>)` | `reader<T>` | output |
| `consumer<T,F>` | `void(reader<T>)` | `writer<T>` | input |
| `filter<In,Out,F>` | `void(reader<In>, writer<Out>)` | `spawn(reader<In>)→reader<Out>`, `spawn(writer<Out>)→writer<In>` | either |

Factories: `make_producer<T>(f)`, `make_consumer<T>(f)`, `make_filter<In,Out>(f)`.

### Composition with `|`

```cpp
// filter | filter → filter     producer | filter → producer
// filter | consumer → consumer producer | consumer → callable
// reader | filter → reader     filter | writer → writer
// reader | consumer → callable producer | writer → callable

auto pipeline = csp::part::map<int>([](int x){ return x*2; })
              | csp::part::where<int>([](int x){ return x > 5; });
auto r = pipeline.spawn(std::move(source_reader));
```

### Canonical combinator loop

Most filters follow this pattern:
```cpp
auto f = csp::part::make_filter<In, Out>([](reader<In> r, writer<Out> w) {
    for (In v; r >> v;) {
        if (!(w << transform(v))) break;
    }
});
```

## Combinator Reference

All in `namespace csp::part` (included via `csp.h`).

| Combinator | Kind | Description |
|---|---|---|
| `batch<T>(n)` | filter | Collect n elements into `vector<T>` |
| `blackhole<T>()` | consumer | Discard all values |
| `buffer<T>(n)` | filter | Bounded async FIFO buffer (size n) |
| `chain<T>(readers...)` | producer | Concatenate readers sequentially |
| `collect<T>(iter)` | consumer | Consume stream into output iterator |
| `concat_all<T>` | filter | Flatten `reader<reader<T>>` sequentially |
| `combine_latest<Ts...>(readers...)` | producer | Emit tuple of latest values whenever any input updates |
| `conflate<T>(f)` | filter | Merge pending values when downstream is slow |
| `count<T>(start,stop,step)` | producer | Numeric sequence [start,stop) |
| `count_forever<T>(start,step)` | producer | Unbounded numeric sequence |
| `deaf<T>()` | consumer | Never-accepting endpoint |
| `debounce<T>(dur)` | filter | Emit after quiet period, suppress rapid fire |
| `default_if_empty<T>(val)` | filter | Emit default if input closes empty |
| `delay<T>(dur)` | filter | Delay each value independently |
| `distinct<T>()` | filter | Suppress consecutive duplicates |
| `enumerate<T>(container)` | producer | Stream container elements |
| `exhaust_all<T>` | filter | Flatten sub-streams, ignoring new while active |
| `fanout<T>(n)` | filter | Broadcast to dynamic subscriber set |
| `first<T>(n)` | filter | Take first n elements |
| `flat_map<In,Out>(f)` | filter | Map to sub-streams, merge results |
| `flatten<T>` | filter | Flatten `vector<T>` → T |
| `gate<T>()` | function | Pause/resume via control channel |
| `group_by<T,K>(f)` | producer | Partition by key, emit (key, reader) pairs |
| `interleave<T>(readers...)` | producer | Strict round-robin interleave |
| `join<T>(readers...)` | function | Block until all channels close |
| `killswitch<T>()` | filter | Forward until keepalive dies |
| `last<T>(n)` | filter | Buffer; emit last n on close |
| `latch<T>()` | filter | Serve most recent value on demand |
| `map<In,Out>(f)` | filter | Transform each element |
| `merge<T>(readers...)` | producer | Non-deterministic merge |
| `mux(reader<Ts>...)` | producer | Heterogeneous merge into `variant<Ts...>` |
| `demux(reader<variant<Ts...>>)` | function | Split variant stream into N typed readers |
| `metrics<T>()` | function | Passthrough with stats reporting |
| `mute<T>()` | producer | Never-producing endpoint |
| `nwise<T>(n)` | filter | Sliding n-element window as tuple |
| `pace<T>(trigger)` | filter | Rate-limited passthrough: one value per trigger, backpressure on excess |
| `pairwise<T>` | filter | Consecutive pairs (a,b), (b,c)... |
| `parallel_map<A,B>(n,f,cfg)` | filter | Concurrent N-worker transform; `cfg.ordered` preserves input order |
| `partition<T>(n,f)` | function | Route to N outputs by classifier |
| `quantize<T>(f)` | function | Variable-size batching |
| `reduce<T,A>(init,f)` | filter | Fold to single value |
| `round_robin<T>(n)` | function | Distribute across N outputs |
| `rpc_client` | function | Request/reply client (two variants) |
| `rpc_server` | function | Request/reply server (two variants) |
| `sample<T,S>(trigger)` | producer | Emit latest value on trigger |
| `scan<In,Out>(init,f)` | filter | Running accumulator |
| `share<T>(n)` | producer | Multicast with latch semantics |
| `sink<T>(f)` | consumer | Consume with side-effect function |
| `skip_first<T>(n)` | filter | Drop first n elements |
| `skip_last<T>(n)` | filter | Emit all but last n |
| `skip_while<T>(pred)` | filter | Drop while predicate true |
| `slide<T>(params)` | function | Sliding window with expiry |
| `stride<T>(n)` | filter | Every Nth element |
| `switch_all<T>` | filter | Flatten sub-streams with latest-wins cancellation |
| `take_while<T>(pred)` | filter | Forward while predicate true |
| `tee<T>(side_writer)` | filter | Duplicate: main first, then side |
| `throttle<T>(trigger,n)` | filter | Rate-limit: n per trigger, use with `tick(d)` |
| `timeout<T>(dur)` | filter | Close if no value within duration |
| `timer(control)` | producer | Sleep per control, emit fire times |
| `try_map<A,B>(f,err)` | filter | Map with exception catching; errors to side channel |
| `unique<T>(cap)` | filter | All-time dedup with optional eviction |
| `unzip<Tuple>()` | function | Split tuple stream into N streams |
| `where<T>(pred)` | filter | Filter by predicate |
| `window<T>(n)` | filter | Sliding window as `vector<T>` |
| `zip<Ts...>(readers...)` | producer | Element-wise zip |

## Gotchas

1. **Move-only endpoints**: `writer<T>` and `reader<T>` have deleted copy
   constructors. Use `std::move()` when passing to spawn/lambdas. Use
   `.copy()` for deliberate shared ownership.

2. **`after()` is non-negative**: `after(d)` sends a `time_point` before
   dying. In prialt, the timeout case is a non-negative match (e.g.,
   `case 1:`), not a death event (`case ~1:`).

3. **`~endpoint` returns complemented**: Death-watch operations (`~w`, `~r`)
   return complemented indices (`~k`) when they fire, just like implicit death
   on regular read/write ops. All death events are complemented.

4. **chan_op blocks in destructor**: `w << val;` as a statement blocks
   because `chan_op`'s destructor calls `prialt`. To avoid blocking, capture
   the return: `auto op = w << val; op.disarm();`.

5. **`spawn(f)` takes f by value**: The callable is moved into the
   imp. Ensure captured state is either moved or intentionally
   shared (via `shared_ptr` or `.copy()`).

6. **Reader range-for copies**: `for (T v : reader)` copies each value.
   Use `for (T& v : reader)` only for const access (iterator stores T).

7. **M:N runtime required**: Timers, I/O, signals, and `blocking()` all
   require `init_runtime()` before use.

8. **Part spawn() consumes endpoints**: `filter.spawn(std::move(r))`
   takes the reader by value. Forgetting `std::move()` won't compile.

9. **stream_to checks writer death**: `r.stream_to(w)` uses
   `prialt(~out, in >> t)` internally, so it stops when the writer's
   reader dies — not just when the input reader dies.

10. **Dynamic bindings must use `local`**: `var = val` returns a deferred
    binding, not a mutation. Always wrap in `csp::local l{var = val}`.
    Bare `var = val;` asserts in debug builds.

11. **Dynamic scoping is per-imp**: Bindings via `local` use
    COW (path-copy HAMT). Changes are invisible to other imps
    unless explicitly shared via `context::current()` + `context_scope`.

## Integration

Copy these files into your project:

| File | Purpose |
|---|---|
| `csp.h` | Single header (all public API) |
| `csp.cpp` | Implementation + context-switching assembly |
| `csp_globals.cpp` | Thread-local state (**must** be a separate translation unit — see [docs/tls-caching-bug.md](https://github.com/marcelocantos/csp/blob/master/docs/tls-caching-bug.md)) |
| `AGENTS-CSP.md` | This file — agent reference for CSP |

Compile with C++17 and libc++:

```bash
c++ -std=c++17 -O2 -c csp.cpp -o csp.o
c++ -std=c++17 -O2 -c csp_globals.cpp -o csp_globals.o
```

Reference this file from your project's `CLAUDE.md` or `AGENTS.md` to
give coding agents CSP expertise.
