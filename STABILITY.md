# Stability

CSP follows [semantic versioning](https://semver.org/). Once 1.0 ships,
backwards-incompatible changes to the public API require a new product fork
(there is no v2.0). The pre-1.0 period exists to get the interaction surface
right before making that commitment.

## Interaction surface catalogue

Everything below is in `namespace csp` unless noted otherwise. Internal
namespaces (`csp::internal`, `csp::detail`) are not part of the public API.

### Core types

| Symbol | Kind | Stability |
|--------|------|-----------|
| `chan<T>` | struct template (`w`, `r`, `release()`) | Stable |
| `writer<T>` | class template (move-only endpoint) | Stable |
| `reader<T>` | class template (move-only endpoint, iterable) | Stable |
| `weak_writer<T>` | class template | Stable |
| `weak_reader<T>` | class template | Stable |
| `chan_op<T>` | class template (RAII channel operation) | Stable |
| `range<T>` | class template (reader + exception propagation) | Stable |
| `poke_t` / `poke` | empty-message surrogate | Stable |
| `none_t` / `none` | non-blocking guard for alt/prialt | Stable |
| `error` | base exception (`std::runtime_error`) | Stable |
| `required<T>` | config field wrapper | Stable |
| `byte` / `bytes` | type aliases (`uint8_t`, `vector<byte>`) | Stable |

### Type aliases

| Symbol | Definition | Stability |
|--------|------------|-----------|
| `time_point` | `std::chrono::steady_clock::time_point` | Stable |
| `duration` | `std::chrono::steady_clock::duration` | Stable |

### Runtime management

| Function | Stability |
|----------|-----------|
| `void init_runtime(int num_procs = 0)` | Stable |
| `void shutdown_runtime()` | Stable |
| `void schedule()` | Stable |
| `void set_scheduler(std::function<void()>)` | Stable |
| `void reset_scheduler()` | Stable |
| `void yield()` | Stable |

### Channel creation and manipulation

| Function | Stability |
|----------|-----------|
| `make_channel(writer<T>&, reader<T>&)` | Stable |
| `operator--(writer<T>&)` — make channel for writer | Stable |
| `operator++(reader<T>&)` — make channel for reader | Stable |
| `swap(writer<T>&, writer<T>&)` | Stable |
| `swap(reader<T>&, reader<T>&)` | Stable |
| `swap(writer<T>&, reader<T>, writer<T>, reader<T>&)` | Needs review |
| `fuse(writer<T>&, reader<T>&)` | Needs review |
| `tap(writer<T>&, reader<T>&)` | Needs review |
| `splice(writer<T>&, reader<T>&, F&&)` | Needs review |

### Spawning and joining

| Function | Stability |
|----------|-----------|
| `spawn(F&&) → reader<exception_ptr>` | Stable |
| `join(reader<exception_ptr> const&)` | Stable |
| `join(R&&)` (range of handles) | Stable |
| `join(Rs&&...)` (variadic handles) | Stable |
| `spawn_consumer<T>(F) → writer<T>` | Stable |
| `spawn_producer<T>(F&&) → reader<T>` | Stable |
| `spawn_filter<T>(F&&) → chan<T>` | Stable |
| `spawn_range<T>(F) → range<T>` | Stable |

### Multiplexing

| Function | Stability |
|----------|-----------|
| `alt(Ops&&...) → int` | Stable |
| `alt(vector<chan_op<T>>&) → int` | Stable |
| `alt(vector<chan_op<T>>&, none_t) → int` | Stable |
| `prialt(Ops&&...) → int` | Stable |
| `prialt(vector<chan_op<T>>&) → int` | Stable |
| `prialt(vector<chan_op<T>>&, none_t) → int` | Stable |

### Globals

| Symbol | Stability |
|--------|-----------|
| `global_exception_handler` (`reader<exception_ptr>`) | Needs review |
| `skip` (`reader<> const`) | Stable |

### Timers (`include/csp/timer.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `clock_source` | abstract base class | Stable |
| `fake_clock` | testing clock | Stable |
| `clock` | `dynamic<clock_source*>` | Stable |
| `now() → time_point` | function | Stable |
| `sleep_until(time_point)` | function | Stable |
| `sleep(duration)` | function | Stable |
| `after(duration) → reader<time_point>` | function | Stable |
| `tick(duration) → reader<time_point>` | function | Stable |

### Cancellation (`include/csp/cancel.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `canceled` | exception type | Stable |
| `timed_out` | exception type (inherits `canceled`) | Stable |
| `cancel_guard` | RAII scope | Stable |
| `cancellation() → cancel_guard` | function | Stable |
| `cancellation(duration) → cancel_guard` | function | Stable |
| `cancellation(time_point) → cancel_guard` | function | Stable |
| `done() → chan_op<>` | function | Stable |
| `is_cancel_active() → bool` | function | Stable |
| `cancel_reason() → exception_ptr` | function | Stable |

### Non-blocking I/O (`csp::io`)

| Function | Stability |
|----------|-----------|
| `wait_readable(int fd)` | Stable |
| `wait_writable(int fd)` | Stable |
| `set_nonblock(int fd) → int` | Stable |
| `read(int fd, void*, size_t) → ssize_t` | Stable |
| `write(int fd, const void*, size_t) → ssize_t` | Stable |
| `accept(int, sockaddr*, socklen_t*) → int` | Stable |
| `connect(int, const sockaddr*, socklen_t) → int` | Stable |
| `resolve(string, string, addrinfo*) → resolve_result` | Stable |

### Blocking pool (`include/csp/blocking.h`)

| Function | Stability |
|----------|-----------|
| `blocking(Fn&&) → invoke_result_t<Fn>` | Stable |

### Dynamic scoping (`include/csp/dynamic.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `dynamic<T>` | class template | Stable |
| `local` | RAII binding scope | Stable |
| `imp_local<T>` | class template | Stable |
| `context_key` | unique key type | Stable |
| `context` | HAMT root handle | Stable |
| `context_scope` | RAII installer | Stable |

### Signals (`csp::signal`)

| Function | Stability |
|----------|-----------|
| `notify(initializer_list<int>) → reader<int>` | Stable |

### TLS (`csp::tls`, behind `#ifdef CSP_TLS`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `tls::error` | exception type | Stable |
| `tls::context` | TLS context (pImpl) | Stable |
| `tls::context::role` | enum (`client`, `server`) | Stable |
| `tls::conn` | TLS connection (pImpl) | Stable |

### Byte reader (`include/csp/byte_reader.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `byte_reader` | class (file-like interface over `reader<bytes>`) | Stable |

### Supervision (`include/csp/supervisor.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `restart_policy` | struct | Fluid |
| `max_restarts_exceeded` | exception type | Fluid |
| `worker_group` | class | Fluid |

### Stack analysis (`include/csp/stack_analysis.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `stack_analysis` | result struct | Needs review |
| `stack_analysis_options` | config struct | Needs review |
| `analyze_stack_depth(...)` | function | Needs review |
| `analyze_stack_depth_cached(...)` | function | Needs review |

### Stream combinators (`csp::part`)

Core infrastructure (`part.h`):

| Symbol | Kind | Stability |
|--------|------|-----------|
| `filter<In, Out, F>` | composable filter | Stable |
| `producer<T, F>` | composable producer | Stable |
| `consumer<T, F>` | composable consumer | Stable |
| `make_filter<In, Out>(F&&)` | factory | Stable |
| `make_producer<T>(F&&)` | factory | Stable |
| `make_consumer<T>(F&&)` | factory | Stable |
| `operator\|` (8 overloads) | composition | Stable |

Combinator catalogue (all Stable unless noted):

**Transformation**: `map`, `try_map`, `foreach_emit`, `scan`, `reduce`,
`pairwise`, `enumerate`, `count`, `count_forever`, `flatten`, `cycle`.

**Filtering**: `where`, `distinct`, `unique`, `take_while`, `take_until`,
`skip_while`, `skip_first`, `skip_last`, `first`, `last`, `stride`.

**Predication**: `any_of` / `all_of` (in `quantify.h`).

**Windowing**: `batch`, `nwise`, `window`, `slide`, `chunk_by`,
`default_if_empty`.

**Merging**: `merge`, `merge_all`, `concat_all`, `exhaust_all`, `zip`,
`combine_latest`, `mux`, `sort_merge`.

**Routing**: `round_robin`, `partition`, `interleave`, `fanout`, `demux`,
`unzip`, `transpose`.

**Temporal**: `delay`, `pace`, `throttle`, `debounce`, `sample`, `timeout`,
`conflate`, `latch`, `gate`, `killswitch`, `timer` (part).

**Fan-out / sub-streams**: `flat_map`, `group_by`, `switch_all`, `tee`,
`fallback`, `share`.

**Parallelism**: `parallel_map`.

**Randomization** (`csp::part::rand`): `uniform_int`, `uniform_real`,
`bernoulli`, `normal`, `choice`, `random_bytes`, `shuffle`.

**I/O** (`csp::part::io`): `byte_reader`, `byte_writer`, `split_lines`,
`fixed_frames`.

**RPC** (`csp::part::rpc`): `rpc_client` (2 overloads), `rpc_server`
(2 overloads).

**Terminal**: `blackhole`, `mute`, `deaf`, `collect`, `sink`, `join`,
`first_wins`.

**Monitoring**: `metrics`, `quantize` (2 overloads + spawn variants).

**Buffering**: `buffer`.

## Gaps and prerequisites

Items that must be addressed before 1.0:

1. **Supervision API redesign** — `worker_group` / `restart_policy` /
   `max_restarts_exceeded` are being replaced by a dynamic-scope-based imp
   death interception mechanism (`imp_exit`). The new API (`on_exit`,
   `supervised`, `exit_guard`, `imp_event`) is implemented but not yet
   merged. This is the primary blocker for API stability.

2. **Version macros** — The library has no `CSP_VERSION` /
   `CSP_VERSION_MAJOR` / `CSP_VERSION_MINOR` / `CSP_VERSION_PATCH` macros.
   These should be added before 1.0 so downstream code can feature-detect.

3. **`global_exception_handler`** — Exposed as a bare
   `reader<exception_ptr>` extern. Consider whether this should be a
   function-based API instead. Current design leaks implementation detail.

4. **`swap`/`fuse`/`tap`/`splice` channel manipulation** — These functions
   exist but are lightly tested and the 4-arg swap signature is unusual.
   Review whether these belong in the public API or should be internal-only.

5. **Stack analysis API** — ARM64-only, exposed publicly but primarily an
   internal optimisation. Consider whether this should be public or moved
   behind a feature flag / internal namespace.

6. **Documentation** — Per-part reference pages exist for some combinators
   but coverage is incomplete. All public API functions need at minimum a
   brief doc comment in the header.

7. **Distribution packaging** — `dist/` ships three files but no install
   target, pkg-config, or CMake find-module. Users must manually integrate.

## Out of scope for 1.0

- **one_for_all / rest_for_one** supervision strategies — deferred until the
  imp_exit API is stable and proven in use.
- **Buffered channels** — design exists but not prioritised.
- **circuit_breaker**, **singleflight**, **bulkhead** resilience combinators.
- **CMake build system** — Makefile is sufficient for now.
- **Windows / Linux** platform support — macOS-only for initial release.
- **C API / FFI** — no stable C interface planned for 1.0.
