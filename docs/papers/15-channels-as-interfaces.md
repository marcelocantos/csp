# Channels as Interfaces

## The core observation

Traditional concurrent systems compose through APIs: typed function
signatures that a caller invokes and a callee implements. CSP channels
offer something different. Because imps and channels are cheap, and
because channel endpoints exhibit **bidirectional lifecycle
observability** — each endpoint's death is independently observable by
the other side — it becomes natural to replace API boundaries with
channel topology.

A component is not something you call. It is something you wire to.

## Bidirectional lifecycle observability

This is the property that makes everything else possible. In CSP,
channel endpoints have independent lifecycles with independent
refcounts. When a writer dies, the reader observes it structurally
via `~writer` in a prialt. When a reader dies, the writer observes
it the same way. Both directions, both through the same mechanism
used for ordinary data flow.

This is fundamentally different from Go channels, where closing a
channel is a one-way signal (writer to reader). A dead reader in Go
is invisible to the writer — the write either blocks forever or
panics on a closed channel. There is no structural way for a producer
to know its consumer is gone.

In CSP, death is symmetric and observable. This means:

- **Cleanup cascades through topology.** A dead component's endpoints
  close, which signals its peers, which can respond — perhaps by
  shutting down themselves, propagating the signal further. The whole
  graph self-manages without external coordination.

- **Health is structural, not polled.** You don't need heartbeats,
  health checks, or timeouts to detect a failed dependency. A dead
  peer is a closed endpoint, observable in the same prialt where
  you're doing real work.

- **Teardown is declarative.** You don't call `component.shutdown()`
  and hope it honours the request. You drop your endpoints. The
  component sees the death and decides what to do. Maybe it shuts
  down. Maybe it finds a new peer. The decision is local.

## Components as endpoint bundles

If a channel endpoint is an interface point, then a component's public
surface is a **bundle of typed endpoints**: request channels, response
channels, event streams, lifecycle signals. Spawning a component
means getting back the complementary halves of the channels it uses
internally.

```cpp
struct cache_endpoints {
    writer<request<string, result>> queries;  // send lookups here
    reader<stats>                   metrics;  // optional metrics stream
};

cache_endpoints start_cache(size_t capacity);
```

The factory function creates the channels internally, spawns the imp
with one side, and returns the other side as a typed bundle. The
caller doesn't construct the channels — the component defines its own
interface. This is the natural ownership: the component knows what
endpoints it exposes, just as a server knows what port it listens on.

This bundle *is* the interface. It is a data structure, not a vtable.
It is composed of values, not methods. And because each endpoint
carries its own lifecycle, the bundle is self-describing: you can
observe which parts are alive, react to partial failure, and wire
subsets of the bundle to different consumers.

## Request/response: the `request` type and `rpc`

The simplest channel interface is a pair of endpoints — one for
requests, one for responses. But a naive design with separate request
and response channels breaks under concurrency: if multiple callers
share the request channel, they can steal each other's responses from
the shared response channel.

The solution is to include a reply channel with each request. The
caller creates a one-shot channel, sends the writer half alongside
the request value, and reads the response from their private reader.
CSP formalises this with `request<Req, Resp>`:

```cpp
template <typename Req, typename Resp>
struct request {
    Req value;
    writer<Resp> reply;
};
```

A component that serves request/response traffic exposes a single
endpoint:

```cpp
writer<request<string, result>> start_cache(size_t capacity);
```

Because `writer<request<Req, Resp>>` is a callable, the simplest
usage looks like a function call:

```cpp
auto val = cache("key");        // send request, block for response
```

This works because `writer::operator()` is defined for request types:
it creates a one-shot reply channel, sends the request, and blocks
for the response. The channel endpoint *is* the API.

For non-blocking usage, `call` returns a `reader<Resp>` instead of
blocking:

```cpp
auto r = call(cache, "key");    // non-blocking: fire the request
auto val = r.read();            // blocking: wait for the response
```

Because `call` returns a reader, callers retain full control:

- **Block immediately**: `call(w, key).read()` (or just `w(key)`)
- **Multiplex**: `prialt(call(w, key) >> val, ~other, ...)`
- **Fan out**: fire multiple calls, collect responses in any order:
  ```cpp
  auto r1 = call(w, k1);
  auto r2 = call(w, k2);
  auto v1 = r1.read();
  auto v2 = r2.read();
  ```

The server side is equally natural — it reads `request` values and
writes responses to the embedded reply channel:

```cpp
spawn([r = std::move(ch.r)] {
    request<string, result> req;
    while (r >> req) {
        req.reply << lookup(req.value);
    }
});
```

For concurrent request handling, the server spawns per-request:

```cpp
while (r >> req) {
    spawn([req = std::move(req)] {
        req.reply << expensive_lookup(req.value);
    });
}
```

This pattern composes naturally with endpoint bundles. A component
can expose a mix of `request` channels (for query/response traffic),
plain channels (for event streams), and lifecycle endpoints — all
in a single typed bundle.

## What channels give you that APIs don't

**Backpressure is built in.** A function call either blocks the caller
(synchronous) or requires explicit async machinery (futures, callbacks,
queues). A channel write naturally blocks when the consumer can't keep
up. No rate limiters, no queue overflow, no retry logic. The system
self-regulates through the topology.

**Multiplexing is first-class.** With prialt, a client can wait on
responses from multiple components simultaneously, including their
death signals. Try doing that with function calls — you end up
reinventing select/poll over futures, and death detection requires a
separate error channel.

**Composition is wiring, not wrapping.** To compose two API-based
components, you write glue code that calls one and feeds the result
to the other. To compose two channel-based components, you fuse
their endpoints. The components don't know about each other. The
wiring is external and reconfigurable.

## Fusing operations as wiring operators

This is where CSP's topology surgery primitives — fuse, splice, swap,
tap — reveal their deeper purpose. They are not just utilities for
manipulating channels. They are the **composition operators** for
connecting component interfaces.

- **Fuse** (`w | r`): Connect a writer from component A to a reader
  on component B. This is the basic wiring operation — "A's output
  feeds B's input." The `|` operator unifies fuse with combinator
  composition: both mean "connect output to input," whether at the
  endpoint level or the pipeline level.

- **Splice**: Insert a component into an existing connection. A
  filter, a logger, a rate limiter — spliced in without either
  endpoint knowing.

- **Swap**: Replace a component's endpoint with a different one.
  Hot-swap a cache implementation. Redirect traffic to a new version.
  The component on the other side sees a brief death-and-reconnection,
  or nothing at all if the swap is atomic.

- **Tap**: Observe a channel non-destructively. Monitoring, debugging,
  metrics collection — without modifying the data path.

These operations work because channels are first-class values with
independent lifecycle. You can manipulate the wiring at runtime
without stopping the system. The topology is a live, mutable graph.

## The vision: topology as architecture

In a traditional system, the architecture is expressed in code: which
module imports which, which class holds a reference to which service,
which function calls which. Changing the architecture means changing
the code.

In a channel-based system, the architecture is expressed in topology:
which endpoints are wired to which. The code inside each component is
independent — it reads from its channels and writes to its channels.
The topology is configured externally, at spawn time, and can be
reconfigured at runtime via fuse/splice/swap.

This is analogous to Unix pipes, but with crucial differences:

- **Typed**: Each channel carries a specific message type. The
  compiler enforces protocol compatibility at wire-up time.

- **Bidirectional lifecycle**: Both ends observe each other's death.
  Unix pipes only signal writer-to-reader (SIGPIPE / EOF).

- **Multi-channel**: A component can expose many endpoints, not just
  stdin/stdout/stderr. The bundle is arbitrarily rich.

- **Dynamic**: The topology can change at runtime. Unix pipelines are
  static once launched.

The result is a system where components are loosely coupled not by
abstraction layers or dependency injection, but by the physical
structure of their communication channels. The wiring *is* the
architecture, and the fusing operations are how you build and
modify it.

## Open questions

**Discoverability.** With an API, you read the type signature. With
an endpoint bundle, you read a struct definition. This works, but
lacks the tooling ecosystem that APIs enjoy (autocomplete, docs
generation, interface checking). Can endpoint bundles be made as
discoverable as method signatures?

**Conventions vs. library support.** Is the factory-function-returning-
a-bundle pattern sufficient, or should the library formalise it?
A `service<Interface>` type could enable generic wiring, composition
combinators, and topology introspection. But it adds complexity.
The right answer probably depends on how the pattern evolves in
practice.

**Error channels.** Should endpoint bundles conventionally include an
error or exception stream? Or is death-observation sufficient? In
many cases, a component dying *is* the error signal, and the exception
is recoverable from the spawn handle. But richer error reporting
might want a dedicated `reader<error>` in the bundle.

**Versioning.** If a component's interface is a struct of endpoints,
how do you evolve it? Adding a new endpoint is additive (old clients
ignore it). Removing one is breaking (old clients hold a dead
endpoint). This mirrors API versioning but in a structural rather
than nominal form.
