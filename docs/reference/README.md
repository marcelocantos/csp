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
| [Scheduling](scheduling.md) | `csp.h` | `spawn`, `schedule`, `yield`, `init_runtime`, `spawn_producer`, `spawn_consumer`, `spawn_filter` |
| [Multiplexing](multiplexing.md) | `csp.h` | `alt`, `prialt`, death-watch (`~reader`, `~writer`), `skip`, two-phase protocol |

## Time, I/O, and OS Integration

| Document | Header | Contents |
|---|---|---|
| [Timers](timers.md) | `csp/timer.h` | `sleep`, `sleep_until`, `after`, `tick` |
| [I/O](io.md) | `csp/io.h` | `wait_readable`, `wait_writable`, `read`, `write`, `accept`, `connect`, `resolve` (kqueue reactor) |
| [Signals](signals.md) | `csp/signal.h` | `signal::notify` (self-pipe trick, reactor-driven) |
| [Blocking](blocking.md) | `csp/blocking.h` | `blocking(fn)` (offload to OS thread pool) |

## Scoping

| Document | Header | Contents |
|---|---|---|
| [Dynamic Scoping](dynamic.md) | `csp.h` | `dynamic<T>`, `local`, `context`, `context_scope`, `mt_local<T>` |

## Stream Combinators

| Document | Header | Contents |
|---|---|---|
| [Combinator Framework](combinators.md) | `csp.h` | `producer<T>`, `filter<In,Out>`, `consumer<T>`, factory functions, pipe operator (`\|`) |
| [Parts Catalog](parts.md) | `csp.h` | 50+ stream combinators: `map`, `where`, `scan`, `merge`, `zip`, `flat_map`, `batch`, `throttle`, `group_by`, ... |
| [Parts (individual)](parts/) | `csp.h` | One page per combinator with signature, description, and example |

## Concepts

The reference documents assume familiarity with a few core ideas:

**Microthreads** are lightweight, cooperatively scheduled execution contexts.
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
channels and microthreads needed to run the pipeline.
