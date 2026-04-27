# CSP Reference

Precise specification of every public type and function in the CSP library.

Each document describes its elements with C++ signatures, natural English
prose, Mermaid state diagrams (where lifecycle warrants), and formal
[transition rules](transition-rules.md) using labeled notation:

```
state.operation(args) ─┤guard├──➤ effects; result
```

## Core

| Document | Header | Contents |
|---|---|---|
| [Channels](channels.md) | `csp.h` | `chan<T>`, `writer<T>`, `reader<T>`, `chan_op<T>`, `poke_t`, rendezvous protocol |
| [Scheduling](scheduling.md) | `csp.h` | `spawn`, `schedule`, `yield`, `set_maxprocs`, `spawn_producer`, `spawn_consumer`, `spawn_filter` |
| [Multiplexing](multiplexing.md) | `csp.h` | `alt`, `prialt`, death-watch (`~reader`, `~writer`), `skip`, two-phase protocol |

## Time, I/O, and OS Integration

| Document | Header | Contents |
|---|---|---|
| [Timers](timers.md) | `csp.h` | `sleep`, `sleep_until`, `after`, `tick` |
| [I/O](io.md) | `csp.h` | `fd_t`, `wait_readable`, `wait_writable`, `read`, `write`, `accept`, `connect`, `resolve`, `read_all`, `write_all`, `lines`; `file::read`, `file::write` |
| [Networking](net.md) | `csp.h` | `net::connection`, `net::listen`, `net::dial` (TCP) |
| [HTTP/1.1](http.md) | `csp/http.h` | `http::serve`, `http::request`, `http::response`, `http::endpoint`, `http::fetch`, `http::get`, `http::post` |
| [Signals](signals.md) | `csp.h` | `signal::notify` (self-pipe trick, reactor-driven) |
| [Blocking](blocking.md) | `csp.h` | `blocking(fn)` (offload to OS thread pool) |

## Security

| Document | Header | Contents |
|---|---|---|
| [TLS](tls.md) | `csp.h` | `tls::context`, `tls::conn`, `tls::error` (cancel-aware TLS 1.3 via PicoTLS, `#ifdef CSP_TLS`) |

## Networking

| Document | Header | Contents |
|---|---|---|
| [WebSocket](websocket.md) | `csp.h` | `ws::upgrade`, `ws::connect`, `ws::conn`, `ws::message`, `ws::opcode` |

## Supervision

| Document | Header | Contents |
|---|---|---|
| [Imp Exit](imp-exit.md) | `csp.h` | `supervised`, `on_exit`, `exit_guard`, `imp_event`, `restart_policy`, `max_restarts_exceeded` |
| [Supervision](supervisor.md) | `csp.h` | `worker_group`, `worker_max_restarts_exceeded` (deprecated -- prefer `on_exit` + `supervised`) |

## Scoping

| Document | Header | Contents |
|---|---|---|
| [Dynamic Scoping](dynamic.md) | `csp.h` | `dynamic<T>`, `local`, `context`, `context_scope`, `imp_local<T>` |

## Stream Combinators

| Document | Header | Contents |
|---|---|---|
| [Combinator Framework](combinators.md) | `csp.h` | `producer<T>`, `filter<In,Out>`, `consumer<T>`, factory functions, pipe operator (`\|`) |
| [Parts Catalog](parts.md) | `csp.h` | 50+ stream combinators: `map`, `where`, `scan`, `merge`, `zip`, `flat_map`, `batch`, `throttle`, `group_by`, ... |
| [Parts (individual)](parts/) | `csp.h` | One page per combinator with signature, description, and example |

## Concepts

The reference documents assume familiarity with a few core ideas:

**Imps** are lightweight, cooperatively scheduled execution contexts.
They run on a small number of OS threads (M:N model) and context-switch only
at well-defined points: channel operations, `yield`, `sleep`, and I/O waits.

**Channels** are unbuffered and synchronous. A write blocks until a reader is
ready; a read blocks until a writer is ready. The value is moved directly
from source to destination with no intermediate storage.

**Endpoints** (`writer<T>` and `reader<T>`) are move-only, reference-counted
handles to channel sides. An endpoint is in one of three states:

| State | `operator bool()` | Meaning |
|-------|-------------------|---------|
| null  | `false` | Default-constructed or moved-from. Not connected. |
| live  | `true`  | Connected to a channel whose peer side may still exist. |
| dead  | `true`  | Connected, but all peer endpoints have been destroyed. |

**Multiplexing** via `alt`/`prialt` selects among multiple channel operations.
A death-watch (`~endpoint`) fires when the peer side is destroyed. The return
value is the matched index, or `~index` for death-watches.

**Combinators** (`namespace csp::part`) are lazy wrappers composed with the `|`
pipe operator. Nothing executes until `.spawn()` is called, which creates the
channels and imps needed to run the pipeline.
