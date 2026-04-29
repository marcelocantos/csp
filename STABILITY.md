# Stability

CSP follows [semantic versioning](https://semver.org/). Once 1.0 ships,
backwards-incompatible changes to the public API require a new product fork
(there is no v2.0). The pre-1.0 period exists to get the interaction surface
right before making that commitment.

Snapshot as of v0.12.0.

## Interaction surface catalogue

Everything below is in `namespace csp` unless noted otherwise. Internal
namespaces (`csp::internal`, `csp::detail`) are not part of the public API.

### Version macros

| Symbol | Value | Stability |
|--------|-------|-----------|
| `CSP_VERSION` | `"0.12.0"` | Stable |
| `CSP_VERSION_MAJOR` | `0` | Stable |
| `CSP_VERSION_MINOR` | `12` | Stable |
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
| `closer<EP>` — vulture-only endpoint wrapper (CTAD, `operator~`, `operator bool`, `endpoint()`) | Stable |

### In-band exception delivery

`writer<T>::_throw(std::exception_ptr)` sends an exception in place of
a value on the next rendezvous; the reader observes it as a thrown
exception at its `r >> val` / `prialt(...)` call site, and the channel
remains live and continues to carry further values or exceptions.
Buffered channels carry exceptions in-order with values.  See
docs/papers/03-two-phase-prialt.md §8 for the wire mechanism.

| Symbol | Stability |
|--------|-----------|
| `writer<T>::_throw(std::exception_ptr) → chan_op<T>` | Stable |
| `chan_op<T>::take_exception() → exception_ptr` (capture without rethrow) | Needs review |

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
| `fd_t` | opaque fd wrapper (no implicit int conversion) | Stable |
| `wait_readable(fd_t)` | function | Stable |
| `wait_writable(fd_t)` | function | Stable |
| `set_nonblock(fd_t) → int` | function | Stable |
| `read(fd_t, void*, size_t) → ssize_t` | function | Stable |
| `write(fd_t, const void*, size_t) → ssize_t` | function | Stable |
| `accept(fd_t, sockaddr*, socklen_t*) → fd_t` | function (returned fd is non-blocking) | Stable |
| `connect(fd_t, const sockaddr*, socklen_t) → int` | function | Stable |
| `read_all(fd_t) → vector<uint8_t>` | function | Stable |
| `write_all(fd_t, span<const uint8_t>)` | function | Stable |
| `addrinfo_deleter` | struct | Stable |
| `addrinfo_ptr` | unique_ptr alias | Stable |
| `resolve_result` | struct (`info`, `error`, `operator bool`, `message`) | Stable |
| `resolve(string, string, addrinfo*) → resolve_result` | function | Stable |
| `read_request` | alias `request<size_t, bytes>` (🎯T17 Stage 1) | Fluid |
| `source` | alias `writer<read_request>` (🎯T17 Stage 1) | Fluid |
| `errno_error` | class (`csp::error` + `int err()`) | Fluid |
| `fd_source(fd_t) → source` | factory: imp serves sized reads from a non-blocking fd | Fluid |

### File I/O (`csp::file`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `file::read(string) → vector<uint8_t>` | function (blocking pool) | Stable |
| `file::write(string, span<const uint8_t>)` | function (blocking pool) | Stable |

### Blocking pool (`include/csp/blocking.h`)

| Function | Stability |
|----------|-----------|
| `blocking(Fn&&) → invoke_result_t<Fn>` | Stable |

### Dynamic scoping (`include/csp/dynamic.h`)

| Symbol | Kind | Stability |
|--------|------|-----------|
| `dynamic<T>` | class template (main()-safe reads) | Stable |
| `dynamic_binding` | deferred binding | Stable |
| `local` | RAII binding scope (throws `csp::error` outside an imp) | Stable |
| `imp_local<T>` | class template (main()-safe reads; write throws outside an imp) | Stable |
| `context_key` | unique key type | Stable |
| `context` | HAMT root handle (`current()` returns empty outside an imp) | Stable |
| `context_scope` | RAII installer (throws `csp::error` outside an imp) | Stable |

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

### HTTP (`csp::http`, `include/csp/http.h`)

HTTP/1.1 server and client built on llhttp (vendored). Body is buffered
(streaming deferred). Server bypasses `net::listen` internally — see the
project's http-server notes for the rationale.

| Symbol | Kind | Stability |
|--------|------|-----------|
| `http::method` | enum class | Fluid |
| `http::method_name(method)` | function | Fluid |
| `http::response` | struct (`status`, `headers`, `body`) | Fluid |
| `http::request` | struct (method, url, version, headers, `body`, `body_stream`, keep_alive, `respond` writer, `hijack` reader) | Fluid |
| `http::request::header(name)` | case-insensitive header lookup | Fluid |
| `http::request::content_length()` | convenience accessor | Fluid |
| `http::request::drain() → const bytes&` | accumulate body_stream into body (idempotent, 🎯T3.2) | Fluid |
| `http::request::hijack_result` | struct (`fd_t fd`, `bytes leftover`) — protocol-upgrade payload | Fluid |
| `http::endpoint` | struct (`requests`, `remote_addr`) | Fluid |
| `http::serve_options` | struct (`listen`, `max_header_size`, `read_chunk_size`) | Fluid |
| `http::server` | struct (`endpoints`, `port`, `local_addr`) | Fluid |
| `http::serve(port, opts)` | server factory | Fluid |
| `http::serve(addr, port, opts)` | server factory | Fluid |
| `http::fetch_options` | struct (`read_chunk_size`) | Fluid |
| `http::fetch(method, url, headers, body, opts)` | one-shot client call | Fluid |
| `http::get(url, headers, opts)` | convenience wrapper | Fluid |
| `http::post(url, body, headers, opts)` | convenience wrapper | Fluid |

### HTTP/2 (`csp::http2`, `include/csp/http2.h`)

HTTP/2 server (h2c plain-text and h2-over-TLS via ALPN). Streams use the
same `http::request` / `http::response` types as HTTP/1.1 — handler code
is protocol-agnostic. Built on nghttp2 (vendored). Server push supported
via `push_promise()` on the per-connection handle.

| Symbol | Kind | Stability |
|--------|------|-----------|
| `http2::endpoint` | struct (per-connection `streams` + push handle) | Fluid |
| `http2::connection_handle` | struct (server push channel) | Fluid |
| `http2::push_promise(connection_handle&, const http::request&)` | function | Fluid |
| `http2::serve_options` | struct | Fluid |
| `http2::server` | struct (`endpoints`, `port`, `local_addr`) | Fluid |
| `http2::serve(port, opts)` | h2c server factory | Fluid |
| `http2::serve_tls(port, tls::context&, opts)` | h2/h2c-via-ALPN server factory (behind `#ifdef CSP_TLS`) | Fluid |

### WebSocket (`csp::ws`, `include/csp/ws.h`)

RFC 6455 WebSocket server-upgrade and client-connect. Built on wslay
(vendored). Currently `ws://` only — `wss://` deferred. Drop the `send`
writer to trigger a Close handshake (BLO).

| Symbol | Kind | Stability |
|--------|------|-----------|
| `ws::opcode` | enum class (`text`, `binary`, `close`, `ping`, `pong`) | Fluid |
| `ws::message` | struct (`opcode op`, `bytes data`) | Fluid |
| `ws::conn` | struct (`reader<message> recv`, `writer<message> send`) | Fluid |
| `ws::upgrade(http::request&) → conn` | server-side: 101 handshake + hijack the fd | Fluid |
| `ws::connect(const string& url) → conn` | client-side: TCP + opening handshake | Fluid |

### HTTP/3 (`csp::http3`, `include/csp/http3.h`)

**Stage 1 scaffold only — all entry points throw `csp::error("http3: not
yet implemented")`.** API surface drafted to fix the protocol-agnostic
handler contract; integration with QUIC (🎯T3.8) and nghttp3 binding
follow in later stages. Behind `#ifdef CSP_TLS` (HTTP/3 always requires
TLS).

| Symbol | Kind | Stability |
|--------|------|-----------|
| `http3::endpoint` | struct (`reader<http::request> streams`, `string remote_addr`) | Fluid |
| `http3::serve_options` | struct (`max_streams`, `rcvbuf`, `cert_pem`, `key_pem`) | Fluid |
| `http3::server` | struct (`endpoints`, `port`, `local_addr`) | Fluid |
| `http3::serve(port, opts) → server` | factory (stub) | Fluid |
| `http3::fetch_options` | struct | Fluid |
| `http3::response` | struct alias for `http::response` | Fluid |
| `http3::fetch(method, url, headers, body, opts) → response` | one-shot client (stub) | Fluid |
| `http3::get(url, headers, opts) → response` | convenience wrapper (stub) | Fluid |
| `http3::post(url, body, headers, opts) → response` | convenience wrapper (stub) | Fluid |

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
`pairwise`, `enumerate`, `count`, `count_forever`, `flatten`, `cycle`,
`diff`, `frame`.

**Filtering**: `where`, `distinct`, `unique`, `take_while`, `take_until`,
`skip_while`, `skip_first`, `skip_last`, `first`, `last`, `stride`.

**Predication**: `any_of` / `all_of` (in `quantify.h`).

**Windowing**: `batch`, `nwise`, `window`, `slide`, `chunk_by`,
`default_if_empty`.

**Merging**: `merge`, `merge_all`, `concat_all`, `exhaust_all`, `zip`,
`combine_latest`, `mux`, `sort_merge`.

**Routing**: `round_robin`, `partition`, `interleave`, `fanout`, `demux`,
`unzip`, `transpose`, `reorder`.

**Temporal**: `delay`, `pace`, `throttle`, `debounce`, `sample`, `timeout`,
`conflate`, `latch`, `gate`, `killswitch`, `timer` (part).

**Fan-out / sub-streams**: `flat_map`, `group_by`, `switch_all`, `tee`,
`fallback`, `share`.

**Parallelism**: `parallel_map`.

**Randomization** (`csp::part::rand`): `uniform_int`, `uniform_real`,
`bernoulli`, `normal`, `choice`, `random_bytes`, `shuffle`.

**I/O** (`csp::part::io`): `byte_reader`, `byte_writer`, `split_lines`,
`fixed_frames`, `lines`.

**RPC** (`csp::part::rpc`): `rpc_client` (2 overloads), `rpc_server`
(2 overloads).

**Terminal**: `blackhole`, `mute`, `deaf`, `collect`, `sink`, `join`,
`first_wins`, `race`.

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

7. **HTTP surface** — Server and client landed in v0.9.0 with buffered
   bodies only. v0.11.0 added a streaming request body via
   `request::body_stream` + `drain()` (T3.2), HTTP/2 (T3.7), and the
   protocol-upgrade `hijack` channel that WebSocket builds on. Streaming
   response bodies, HTTPS for HTTP/1.1 (over `tls::conn`), HTTP/3
   (T3.9 — currently scaffold only), HTTP client refinements (connection
   reuse, redirects), and unification of HTTP/1.1, HTTP/2 and HTTP/3
   serve-options remain Fluid.

8. **WebSocket surface** — `ws::upgrade` / `ws::connect` landed in
   v0.11.0 (T3.5). Currently `ws://` only — TLS-secured `wss://` deferred.
   The `conn{recv, send}` shape and Close-on-writer-death lifecycle need
   real-world use before freezing.

9. **Pull-based source** — `io::source` / `io::fd_source` (T17 Stage 1)
   landed in v0.11.0 as the foundation for streaming I/O. Higher layers
   (`tls_source`, `http_body_source`) and the `source`-consuming side of
   `http::fetch` / `http::post` are not yet implemented.

10. **HTTP/3** — Stage 1 scaffold in v0.11.0 (nghttp3 vendored, API
    drafted, stubs throw). Implementation depends on QUIC transport
    (T3.8) which is not yet started in master.

11. **Windows portability** — v0.12.0 makes the arena overflow check
    portable (`__debugbreak()` on MSVC, `__builtin_trap()` elsewhere).
    `[[gnu::always_inline]]` produces a recoverable warning, not an
    error, on MSVC. Windows CI compiles cleanly; ws/http3 test suites
    are excluded pending CMake wiring of wslay / nghttp3 / ngtcp2.

## Out of scope for 1.0

- **one_for_all / rest_for_one** supervision strategies — deferred until the
  imp_exit API is stable and proven in use.
- **circuit_breaker**, **singleflight**, **bulkhead** resilience combinators.
- **C API / FFI** — no stable C interface planned for 1.0.
