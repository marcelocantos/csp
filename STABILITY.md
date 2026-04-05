# Stability

CSP follows [semantic versioning](https://semver.org/). Once 1.0 ships,
backwards-incompatible changes to the public API require a new product fork
(there is no v2.0). The pre-1.0 period exists to get the interaction surface
right before making that commitment.

Snapshot as of v0.6.0.

## Interaction surface catalogue

Everything below is in `namespace csp` unless noted otherwise. Internal
namespaces (`csp::internal`, `csp::detail`) are not part of the public API.

### Version macros

| Symbol | Value | Stability |
|--------|-------|-----------|
| `CSP_VERSION` | `"0.6.0"` | Stable |
| `CSP_VERSION_MAJOR` | `0` | Stable |
| `CSP_VERSION_MINOR` | `6` | Stable |
| `CSP_VERSION_PATCH` | `0` | Stable |

### Core types

| Symbol | Kind | Stability |
|--------|------|-----------|
| `chan<T>` | struct template (`w`, `r`, `release()`, `chan(size_t)` buffered ctor) | Stable |
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
| `ClientSide` / `ServerSide` | protocol markers | Stable |
| `incoming<Side, T>` / `outgoing<Side, T>` | protocol type aliases | Stable |
| `request<Req, Resp>` | struct template (request + reply channel) | Fluid |
| `is_request<T>` | trait (`std::true_type` / `std::false_type`) | Fluid |

### Type aliases

| Symbol | Definition | Stability |
|--------|------------|-----------|
| `time_point` | `std::chrono::steady_clock::time_point` | Stable |
| `duration` | `std::chrono::steady_clock::duration` | Stable |

### Runtime management

| Function | Stability |
|----------|-----------|
| `void set_maxprocs(int num_procs = 0)` | Stable |
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
| `channel_swap(writer<T>&, writer<T>&)` | Needs review |
| `channel_swap(reader<T>&, reader<T>&)` | Needs review |
| `call(writer<request<Req, Resp>>&, Req) → reader<Resp>` | Fluid |
| `writer<request<Req, Resp>>::operator()(Req) → Resp` | Fluid |
| `operator\|(writer<T>&, reader<T>&)` — syntactic sugar for `fuse` | Needs review |

### Buffered channel composition

| Function | Stability |
|----------|-----------|
| `reader<T> operator\|(reader<T>, chan<T>)` | Stable |
| `writer<T> operator\|(chan<T>, writer<T>)` | Stable |

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
| `global_exception_handler` (`writer<exception_ptr>`) | Needs review |
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

| Symbol | Kind | Stability |
|--------|------|-----------|
| `socket_t` | type alias (`SOCKET` on Windows, `int` on Unix) | Stable |
| `invalid_socket` | constant | Stable |
| `wait_readable(socket_t)` | function | Stable |
| `wait_writable(socket_t)` | function | Stable |
| `set_nonblock(socket_t) → int` | function | Stable |
| `close(socket_t)` | function | Stable |
| `read(socket_t, void*, size_t) → ssize_t` | function | Stable |
| `write(socket_t, const void*, size_t) → ssize_t` | function | Stable |
| `accept(socket_t, sockaddr*, socklen_t*) → socket_t` | function | Stable |
| `connect(socket_t, const sockaddr*, socklen_t) → int` | function | Stable |
| `addrinfo_deleter` | struct | Stable |
| `addrinfo_ptr` | unique_ptr alias | Stable |
| `resolve_result` | struct (`info`, `error`, `operator bool`, `message`) | Stable |
| `resolve(string, string, addrinfo*) → resolve_result` | function | Stable |

### Blocking pool (`include/csp/blocking.h`)

| Function | Stability |
|----------|-----------|
| `blocking(Fn&&) → invoke_result_t<Fn>` | Stable |

### Dynamic scoping (`include/csp/dynamic.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `dynamic<T>` | class template | Stable |
| `dynamic_binding` | deferred binding | Stable |
| `local` | RAII binding scope | Stable |
| `imp_local<T>` | class template | Stable |
| `context_key` | unique key type | Stable |
| `context` | HAMT root handle | Stable |
| `context_scope` | RAII installer | Stable |

### Unix signals (`csp::signal`)

| Function | Stability |
|----------|-----------|
| `notify(initializer_list<int>) → reader<int>` | Stable |

### Windows signals (`csp::win::signal`, behind `#ifdef _WIN32`)

| Function | Stability |
|----------|-----------|
| `notify(initializer_list<DWORD>) → reader<DWORD>` | Stable |
| `raise(DWORD)` | Stable |

### TLS (`csp::tls`, behind `#ifdef CSP_TLS`)

TLS 1.3 only via PicoTLS minicrypto backend. No built-in X.509 verification.

| Symbol | Kind | Stability |
|--------|------|-----------|
| `tls::error` | exception type (`int code`) | Stable |
| `tls::verify_fn` | `std::function<bool(const char*, const vector<vector<uint8_t>>&)>` | Stable |
| `tls::context` | TLS context (pImpl) | Stable |
| `tls::context::role` | enum (`client`, `server`) | Stable |
| `tls::context::load_cert(const char*)` | load cert chain from PEM file | Stable |
| `tls::context::load_key(const char*)` | load PKCS#8 key from PEM file (secp256r1) | Stable |
| `tls::context::set_verify(verify_fn)` | set custom cert verification callback | Stable |
| `tls::conn` | TLS connection (pImpl) | Stable |

### Byte reader (`include/csp/byte_reader.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `byte_reader` | class (file-like interface over `reader<bytes>`) | Stable |

### Imp exit / supervision (`include/csp/imp_exit.h`, `include/csp/supervisor.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `restart_policy` | struct (`max_restarts`, `window`, `backoff`) | Fluid |
| `max_restarts_exceeded` | exception type | Fluid |
| `imp_event` | struct (`error`, `restart(duration)`) | Fluid |
| `supervised_fn` | class (retry loop callable) | Fluid |
| `supervised(F&&) → supervised_fn` | function template | Fluid |
| `exit_guard` | RAII supervision registration | Fluid |
| `on_exit(function<void(imp_event)>) → exit_guard` | function | Fluid |
| `on_exit(restart_policy) → exit_guard` | function | Fluid |
| `worker_max_restarts_exceeded` | exception type | Fluid |
| `worker_group` | class (deprecated) | Fluid |

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

1. **Supervision API stabilisation** — The new imp exit API
   (`imp_event`, `on_exit`, `exit_guard`, `supervised`) is implemented
   and merged but marked Fluid. It needs real-world usage before
   freezing. `worker_group` is deprecated and should be removed or
   replaced before 1.0.

2. **`global_exception_handler`** — Exposed as a bare
   `writer<exception_ptr>` extern. Consider whether this should be a
   function-based API instead. Current design leaks implementation detail.

3. **`swap`/`fuse`/`tap`/`splice`/`channel_swap` channel manipulation** —
   These functions exist but are lightly tested and the 4-arg swap
   signature is unusual. Review whether these belong in the public API
   or should be internal-only. `channel_swap` may be a backwards-compat
   alias that should be removed.

4. **Stack analysis API** — ARM64-only, exposed publicly but primarily an
   internal optimisation. Consider whether this should be public or moved
   behind a feature flag / internal namespace.

5. **Documentation** — Per-part reference pages exist for some combinators
   but coverage is incomplete. All public API functions need at minimum a
   brief doc comment in the header.

6. **Distribution packaging** — `dist/` ships three files but no install
   target, pkg-config, or CMake find-module. Users must manually integrate.

## Out of scope for 1.0

- **one_for_all / rest_for_one** supervision strategies — deferred until the
  imp_exit API is stable and proven in use.
- **circuit_breaker**, **singleflight**, **bulkhead** resilience combinators.
- **C API / FFI** — no stable C interface planned for 1.0.
