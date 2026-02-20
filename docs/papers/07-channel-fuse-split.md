# Topology Surgery on Live Channels

## Abstract

Synchronous channel systems form static graphs at construction time:
each channel connects one writer group to one reader group, and data
flows along fixed edges. We describe a pair of operations — *fuse* and
*split* — that splice auxiliary channels into a live graph without
modifying application logic. The primary uses are instrumentation
(tapping a channel for logging, metrics, or test assertions),
diagnostics (injecting a recording stage between two pipeline
components), and supervision (resplicing channels around a dead
process to restore the intended topology). Both operations are built
from a single primitive: a four-argument `swap` that exchanges the
channels targeted by two endpoint pairs. A temporary channel, created
and destroyed within the swap, acts as a bridge that redirects traffic
while ensuring that orphaned sides observe clean endpoint death. The
construction requires no new runtime machinery — it composes entirely
from the existing slot-based indirection layer and the per-endpoint
lifecycle model.

## 1. Why touch a running graph?

In a CSP system, channels are created and connected at spawn time.
A pipeline `A → B → C` is wired by passing writers and readers into
successive imp closures. Once running, the graph is fixed — and
usually that is exactly what you want. Static topology is easy to
reason about: data flows along edges that are visible in the source
code, and the structure of the program *is* the structure of the
graph.

The question, then, is not "how do we redesign applications around
dynamic topology" but "what legitimate reasons exist for touching a
graph that is already running?" Three categories stand out.

**Testing and diagnostics.** A test harness wants to tap a channel
between two stages to assert on the values flowing through it, without
modifying the stages themselves. A diagnostic tool wants to splice a
recording imp into a live pipeline to capture a traffic sample. In
both cases the application topology is unchanged — the tooling is
attached from the outside.

**Telemetry and metrics.** A metrics collector can be fused into an
existing channel to count messages, measure latency, or sample
throughput. The application code neither knows nor cares that the
channel has been instrumented. When the collector is removed, the
original topology is restored.

**Supervision and fault recovery.** When a process in a pipeline
dies, its channels become half-dead: the surviving endpoints on the
other sides observe endpoint death. A supervisor that wants to restart
the failed process needs to wire the replacement into the same
position in the graph. Without a splice operation, this requires the
supervisor to tear down and rebuild the entire downstream graph.
With fuse and split, the supervisor resplices the replacement's
endpoints into the existing channels, restoring the intended topology
without disturbing the rest of the pipeline. This is not changing the
application's structure — it is *maintaining* it in the face of
failure.

All three categories share a common trait: the application topology is
a fixed design-time decision; the rewiring serves a non-functional
concern (observability, testability, fault tolerance) that is
orthogonal to the data flow itself.

The challenge is that endpoints are shared. A `writer<T>` can be
copied: multiple imps may hold copies of the same writer, all
targeting the same channel. Any rewiring operation must redirect *all*
copies transparently — an imp holding a stale copy must not silently
write to the wrong channel.

## 2. Slot-based indirection

CSP's endpoint model already solves the sharing problem through an
indirection layer (introduced for the two-argument `swap`). Every
endpoint handle points not to a channel, but to a *slot*:

```
writer copies ──→ Slot ──→ Channel
                  │
writer copies ──→─┘
```

The slot is a separately-allocated structure with two fields:

```cpp
struct Slot {
    std::atomic<void*> channel;    // Channel* (type-erased)
    std::atomic<size_t> refcount;  // live endpoint handles
};
```

All copies of an endpoint share the same slot. The slot's `channel`
pointer can be atomically redirected; when it is, every copy
transparently targets the new channel. The refcount tracks handle
liveness: when it reaches zero, endpoint death is triggered on the
channel the slot currently targets.

The two-argument `swap(a.w, b.w)` exchanges the channel pointers of
two slots under a two-channel lock, then wakes all waiters so they
re-resolve through their slots. This is a bijective permutation of
the topology: if A targeted X and B targeted Y, afterward A targets Y
and B targets X. No channels are created or destroyed; no endpoint
groups gain or lose members.

## 3. Fuse: merging two channel paths

Fuse takes a writer on one channel and a reader on another, and
redirects them onto a shared channel so that data written through the
writer reaches the reader:

```
Before fuse(a.w, b.r):

  a.w ──→ [Channel A] ──→ a.r
  b.w ──→ [Channel B] ──→ b.r

After fuse(a.w, b.r):

  a.w ──→ [Temp Channel] ──→ b.r
           ╳ Channel A ← writer died (a.r observes death)
           ╳ Channel B ← reader died (b.w observes death)
```

The writer side of Channel A and the reader side of Channel B are
orphaned — they have lost the endpoint group that was just redirected.
The surviving endpoints on those channels (`a.r` and `b.w`) observe
clean endpoint death through the standard lifecycle mechanism.

### Implementation

Fuse is expressed as a four-argument swap with empty middle
parameters:

```cpp
template <typename T>
void fuse(writer<T>& w, reader<T>& r) {
    swap(w, {}, {}, r);
}
```

The four-argument swap detects the empty middle parameters and creates
a temporary channel to serve as the bridge:

```cpp
template <typename T>
void swap(writer<T>& w1, reader<T> r1, writer<T> w2, reader<T>& r2) {
    if (!r1 && !w2) {
        chan<T> temp;
        swap(w1, temp.w);   // w1 → temp's channel; temp.w → w1's old channel
        swap(temp.r, r2);   // temp.r → r2's old channel; r2 → temp's channel
        return;
    }
    swap(w1, w2);
    swap(r1, r2);
}
```

The key insight is what happens when `temp` goes out of scope.
`temp.w` now targets w1's original channel (Channel A), and `temp.r`
now targets r2's original channel (Channel B). When the `chan<T>`
destructor runs, it destroys `temp.w` and `temp.r`. Their slots'
refcounts reach zero, triggering endpoint death:

- `temp.w` → writer death on Channel A → `a.r` observes death
- `temp.r` → reader death on Channel B → `b.w` observes death

Meanwhile, `w1` and `r2` both target the temp channel. The temp
channel's `Channel` object is heap-allocated and reference-counted via
the `alive_` field — it survives the stack-allocated `chan<T>` as long
as at least one endpoint group targets it. When both `w1` and `r2`
(and all their copies) are eventually destroyed, the temp channel is
reclaimed.

### Two swaps, not one

Why two sequential swaps rather than a single atomic operation? An
atomic four-way swap would require locking three channels
simultaneously (w1's channel, r2's channel, and the temp channel),
introducing a three-lock ordering problem that the two-argument swap's
ID-ordered locking protocol does not handle.

Two sequential swaps avoid this. The intermediate state — after the
first swap but before the second — is observable: `w1` targets the
temp channel, but `r2` still targets Channel B. A writer sending
through `w1` during this window would block (no reader on the temp
channel yet). This is indistinguishable from normal contention on a
synchronous channel. The window is brief: the two swaps execute
back-to-back with no yield point between them, so in single-threaded
mode the intermediate state is never observed. In M:N mode, it
manifests as a momentary stall — one write's worth of latency at
worst.

## 4. Split: breaking one channel into two

Split is the inverse of fuse. It takes a writer and reader on the
same channel (or any two endpoints whose redirection is desired) and
separates them onto different channels:

```
Before split:

  orig.w ──→ [Channel Orig] ──→ orig.r
  a.w ───→ [Channel A] ───→ a.r
  b.w ───→ [Channel B] ───→ b.r

swap(orig.w, std::move(a.r), std::move(b.w), orig.r)

After split:

  orig.w ──→ [Channel B] ──→ b.r
  a.w ───→ [Channel A] ───→ orig.r
           ╳ Channel Orig ← both sides died
```

The middle parameters (`a.r` and `b.w`) are passed by value. The
four-argument swap takes them by move, so the caller's copies are
consumed. Inside the swap, two two-argument swaps execute:

1. `swap(orig.w, b.w_local)` — orig.w now targets Channel B;
   the local b.w copy now targets Channel Orig.
2. `swap(a.r_local, orig.r)` — the local a.r copy now targets
   Channel Orig; orig.r now targets Channel A.

When the function returns, the local copies (`a.r_local` and
`b.w_local`) are destroyed. Both now target Channel Orig. Their
deaths leave Channel Orig with no writer and no reader — it is
reclaimed.

The surviving endpoints form two independent channels:

- `orig.w` and `b.r` share Channel B
- `a.w` and `orig.r` share Channel A

## 5. The general four-argument swap

Fuse and split are calling patterns of the same primitive. The
four-argument `swap(w1, r1, w2, r2)` dispatches on the emptiness of
the middle parameters:

| `r1` | `w2` | Behaviour |
|------|------|-----------|
| empty | empty | Fuse mode: create temp channel, two swaps, temp dies |
| valid | valid | Split mode: two swaps, middle params die on return |

The by-value middle parameters serve a dual purpose. They carry the
endpoint handles that will be consumed (in split mode), and their
absence signals fuse mode. No flags, no enum, no mode parameter —
the type system and move semantics encode the intent.

## 6. Correctness argument

### No data loss

Synchronous channels have no internal buffer. A value exists in
exactly one place at all times: the sender's buffer or the receiver's
variable. The two-phase prialt protocol holds channel locks across the
match-and-transfer window. A swap cannot interleave with a transfer on
the same channel because swap acquires the same channel locks.
Therefore, a value is never duplicated, dropped, or delivered to the
wrong peer.

### Clean death propagation

The per-endpoint lifecycle model guarantees that death is observable.
When a slot's refcount reaches zero, `resolve_endpoint_death` is
called on the channel the slot currently targets — not the channel it
was originally created for. After a swap, a slot may target a
different channel than it started on. Its death is delivered to the
correct (current) channel.

### No dangling references

The temp channel's `Channel` object survives on the heap as long as
at least one endpoint group targets it (tracked by `alive_`). The
stack-allocated `chan<T>` merely creates the channel; it does not own
it. Endpoint handles own the channel through the refcount and
`alive_` mechanism.

### Pinning prevents use-after-free

A subtle hazard arises when an imp is sleeping in prialt with a
pointer to a channel in its sorted-lock array. If all endpoints on
that channel die while the imp sleeps, the channel is deleted, and the
imp wakes to a dangling pointer. The prialt implementation pins
channels by incrementing `alive_` before sleeping and decrementing
after deregistering. This ensures that the channel survives until the
imp has finished its post-wakeup cleanup.

## 7. Limitations

### Blocked waiters are not redirected

`swap_slots` does not wake waiters that are blocked on a channel whose
slot has just been redirected. A waiter blocked in prialt on
Channel A, whose slot is then swapped to Channel B, remains registered
on Channel A. It will not receive data written to Channel B until
something wakes it from Channel A (typically endpoint death when the
old channel becomes fully orphaned).

This means fuse and split are most naturally applied *before* traffic
begins on the affected channels, or at a quiescent point where no
waiters are actively blocked on the endpoints being redirected. When a
fuse is performed mid-flight under M:N concurrency, the redirected
waiter wakes when the orphaned channel's last endpoint dies, then
re-resolves its slot to the new channel and retries. The latency of
this re-resolution depends on how quickly the orphaned channel's
endpoints are released.

### Same-side constraint

`swap_slots` asserts that both slots are on the same side (both
writers or both readers). Swapping a writer slot with a reader slot is
not supported — the channel's back-pointer update logic assumes
symmetric sides. The four-argument swap respects this: it swaps
writers with writers and readers with readers.

## 8. Related work

Go's channels are immutable values — there is no mechanism to redirect
a channel reference after creation. Topology changes require creating
new channels and passing them through the existing graph, which
requires cooperation from every goroutine that holds a reference.

Erlang's process model achieves dynamic routing through PIDs: a
process can be told to send to a different PID at runtime. This is
analogous to slot indirection, but at the process granularity rather
than the channel granularity. Erlang has no equivalent of fuse or
split as atomic operations.

Kotlin's `Channel` interface supports `close()` but not redirection.
Dynamic topology requires manual channel replacement with explicit
coordination between producers and consumers.

CSP's slot-based indirection makes topology surgery a property of the
endpoint abstraction itself. Fuse and split are not special runtime
operations — they are compositions of the same `swap_slots` primitive
that powers the two-argument swap, using move semantics and RAII
destruction to achieve the desired lifecycle effects.
