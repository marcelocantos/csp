# Channels That Know When to Die

## Abstract

Most channel implementations treat a channel as a single object with
a single lifecycle: it is created, used, and then closed. CSP takes a
different approach. Each channel has two independently reference-counted
endpoints — a writer and a reader — each of which can be closed,
copied, or passed to another imp without affecting the other.
Endpoint death is a first-class event that can be multiplexed alongside
data operations. This paper describes the design, the subtle bug it
revealed, and how the model enables a combinator library of 50+
composable stream transformers that clean up after themselves.

## 1. The problem with whole-channel close

Go's channels have a single lifecycle. You create one with `make(chan
int)`, pass it around, and eventually call `close()`. This works, but
it has sharp edges.

The sender calls `close()`. But which sender? If multiple goroutines
share a channel, they must coordinate to avoid closing it twice (a
panic) or sending on a closed channel (also a panic). The receiver
detects closure via the two-value receive (`v, ok := <-ch`), but the
zero value of `T` is a valid datum for many types — you must always
check `ok`. And once a channel is closed, it's closed for everyone:
there is no way to express "I'm done writing, but other writers may
not be."

Rust's channels (both `std::sync::mpsc` and crossbeam) separate
senders from receivers, which helps. But the lifecycle model is still
binary: the channel is live or it isn't. There is no built-in
mechanism to *select on* the death of a channel alongside data
operations.

The core issue is that channel death is treated as an error condition
— something you check for after the fact — rather than an event you
can anticipate and respond to in the same way you respond to data.

## 2. Independent endpoint lifecycle

In CSP, a `chan<T>` is a struct with two public members:

```cpp
auto ch = csp::chan<int>{};
writer<int>& w = ch.w;
reader<int>& r = ch.r;
```

Or, more commonly:

```cpp
auto [w, r] = csp::chan<int>{};
```

The `writer<T>` and `reader<T>` are **move-only**. You cannot copy
an endpoint with `=` or copy construction. This is deliberate: each
endpoint is a reference-counted handle. Moving transfers ownership.
The underlying channel tracks exactly how many live writers and
readers exist, and uses these counts to determine when each side has
shut down.

When you need shared ownership, you ask for it explicitly:

```cpp
spawn([w = out.copy()] {
    w << 1;
    w << 2;
});
// out is still valid — .copy() incremented the refcount
```

The `.copy()` call makes shared ownership visible at the call site.
A reader scanning the code can see exactly where a channel's lifetime
is extended.

The write side dies when the last writer is destroyed. The read side
dies when the last reader is destroyed. These are independent events.
A channel with no writers but a live reader is a drained stream — the
reader can observe the exhaustion and exit. A channel with no readers
but a live writer is a broken pipe — the writer can observe the
failure and stop producing.

## 3. Death as a first-class event

CSP's multiplexing primitives, `alt` and `prialt`, accept not only
data operations but **death watches** — called vultures:

```cpp
int n;
switch (alt(r >> n, ~w)) {
case  0: /* data arrived on r */   break;
case ~1: /* w's reader has died */ break;
}
```

The `~` operator on an endpoint creates a vulture: `~w` fires when
no readers remain on `w`'s channel (the writer has nobody to send
to), and `~r` fires when no writers remain (the reader will never
receive again). Vultures return bitwise-complemented indices,
distinguishing them from data matches without ambiguity.

This changes the programming model fundamentally. Instead of:

```go
// Go: check after the fact
v, ok := <-ch
if !ok {
    return // channel closed
}
```

You write:

```cpp
// CSP: death is multiplexed alongside data
switch (prialt(~quit_reader, data_reader >> n)) {
case ~0: return;           // quit signal — reader died
case  1: process(n); break;
}
```

The scheduler tells you what happened. You never need to check a
boolean after the fact, and the zero value of `T` is never ambiguous.

## 4. The 50% flake

The per-endpoint lifecycle model exposed a subtle bug that manifested
as a 50% test flake — and the bug, once understood, revealed a design
principle.

### 4.1 The setup

`alt()` randomizes its scan order for fairness: if multiple channels
are ready simultaneously, each has an equal chance of being selected.
`prialt()` scans left to right.

When `prialt_begin_impl` encounters a dead channel during its scan,
the original code returned immediately:

```cpp
for (int k = 0; k < count; ++k) {
    int i = (offset + k) % count;
    if (!*ch) {
        unlock_all();
        out->result = ~i;  // report dead channel
        return;             // ← BUG: skips remaining channels
    }
    // ... check for ready peers ...
}
```

### 4.2 The bug

Consider an `alt` with two operations: a read on a dead channel and
a read on a live channel with a ready peer:

```cpp
alt(dead_reader >> x, live_reader >> y)
```

`alt` randomizes scan order. Half the time it scans the dead channel
first, immediately returns `~0` (dead), and never checks the live
channel — even though a ready peer is waiting. The other half, it
scans the live channel first, finds the ready peer, and returns `1`
(data). The result is a coin flip: either you get the data or you
get a spurious death report.

This manifested as the `Channel - AltIn` test deadlocking ~50% of
the time.

### 4.3 The fix

The fix distinguishes between two kinds of channel operations when
they encounter a dead channel:

- **Data chanops** (`r >> x`, `w << v`): the caller wants data. A
  dead channel is a fallback, not the primary goal. Defer the death
  report until after scanning all channels for ready peers.

- **Vultures** (`~r`, `~w`): the caller is explicitly watching for
  death. Fire immediately.

```cpp
int dead_data_result = 0;
for (int k = 0; k < count; ++k) {
    int i = (offset + k) % count;
    if (!*ch) {
        if (flags & ready_flag) {
            // Data chanop: defer dead-channel result
            if (!dead_data_result) dead_data_result = ~i;
        } else {
            // Vulture: fire immediately
            unlock_all();
            out->result = ~i;
            return;
        }
        continue;
    }
    // ... check for ready peers ...
}

// Only report dead data-chanop after scanning all channels
if (dead_data_result) {
    unlock_all();
    out->result = dead_data_result;
    return;
}
```

### 4.4 The design principle

The bug revealed that death detection has two distinct semantics, and
conflating them causes incorrect behaviour:

1. **Observational death**: "I was trying to send or receive, and
   the channel is dead." This is a fallback — you'd prefer data.

2. **Intentional death**: "I am watching for death." This is the
   primary goal — `~r` exists to detect death, so a dead channel is
   exactly the event you want.

The tilde syntax makes this distinction visible at the call site.
Without per-endpoint death detection, the bug could not arise —
but the feature would also not exist.

## 5. Stream combinators: the payoff

The per-endpoint lifecycle model pays off most dramatically in the
stream combinator library. CSP ships with 50+ composable transformers
in `namespace csp::part`, and they all rely on a single pattern:

```cpp
// The canonical death-aware forwarding loop
for (T v; alt(in >> v, ~out) >= 0;)
    out << transform(v);
```

This loop does three things:

1. **Reads** from the input channel.
2. **Watches** the output endpoint for death (no more readers
   downstream).
3. **Exits** when either the input is exhausted (all writers gone,
   `alt` returns a complemented index for `in`, which is `< 0`) or
   the output is dead (`~out` fires, also `< 0`).

Every combinator — `map`, `where`, `scan`, `flat_map`, `merge`,
`zip`, `debounce`, `throttle`, and 40 more — is a variation on this
loop. The lifecycle management is identical for all of them: read
until you can't, write until nobody's listening, then exit. No
explicit cleanup, no cancellation tokens, no context objects.

This enables composition. Combinators snap together with `operator|`:

```cpp
auto batches = (count(1)
    | map<int>([](int x) { return x * x; })
    | where<int>([](int x) { return x % 2 == 0; })
    | batch<int>(10)
).spawn();

for (auto& vec : batches)
    process(vec);  // each vec is a std::vector<int> of 10 elements
```

`count(1)` is a `producer<int>`; each `|` appends a filter stage;
`.spawn()` materializes the pipeline into running imps and
returns a `reader<std::vector<int>>`. When that reader is dropped
(here, when the for-loop exits), death propagates
backward through the pipeline: each stage's `~out` fires, the stage
exits, its input reader is destroyed, which fires the previous
stage's `~out`, and so on. The entire pipeline tears down
automatically.

## 6. Comparison with Go

A simple forwarding filter in Go:

```go
func filter(in <-chan int, out chan<- int, pred func(int) bool) {
    defer close(out)
    for v := range in {
        if pred(v) {
            out <- v
        }
    }
}
```

The same in CSP:

```cpp
auto filtered = where<int>(pred);
```

The Go version requires explicit `close(out)` — forget it and
downstream blocks forever. The CSP version handles cleanup
through endpoint lifecycle: when the filter's imp exits,
its `writer<int>` is destroyed, the output channel's write side
dies, and any downstream reader observes this via `alt` or loop
termination.

For a pipeline, the difference compounds. Go requires careful
orchestration of `close()` calls, `sync.WaitGroup`, or `context.Context`
to propagate cancellation. CSP pipelines tear down automatically
through endpoint destruction. The programmer writes only the
transformation logic; the library handles the lifecycle.

More importantly, CSP's death detection is *composable with data
operations*. In Go, cancellation and data flow are separate
mechanisms (context vs. channel). In CSP, they are the same mechanism:
you multiplex both in a single `alt` or `prialt` call, and the
scheduler tells you which event occurred.

## 7. Design consequences

The per-endpoint lifecycle model has several consequences that
ripple through the library's design:

**Move-only endpoints prevent accidental lifetime extension.** You
cannot copy a writer and forget about it, accidentally keeping a
channel alive. Shared ownership requires `.copy()`, which is a
deliberate, visible action.

**Death propagation is automatic.** When an imp exits, its
stack is unwound, its endpoints are destroyed, and the reference
counts decrement. If the last writer is destroyed, all blocked
readers unblock. No explicit notification is needed.

**Backpressure is structural.** Because channels are synchronous, a
slow consumer automatically slows the producer. Combined with death
detection, this means a pipeline that loses its sink doesn't just
buffer indefinitely — the death propagates back and shuts down the
sources.

**The type system encodes direction.** A `writer<int>` can only
send; a `reader<int>` can only receive. You cannot accidentally
read from a write endpoint. This eliminates an entire class of
bugs at compile time.

These aren't independent features — they form a coherent design
where typed endpoints, independent lifecycle, death observation, and
synchronous semantics reinforce each other. Remove any one of them
and the combinator library becomes significantly harder to build
correctly.
