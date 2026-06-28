# Paper 31 — tls::stream close_notify shutdown hang

🎯 Target: T17.3
Status: **resolved**. Root cause: `send_close_notify` used a *blocking*
`up_out << alert` write, which deadlocks when two streams tear down over
synchronous (unbuffered) ciphertext transports that neither side is
draining. Fix: send the alert non-blocking and drop it if no reader is
ready.
Filed and resolved: 2026-06-22.

## TL;DR

The deferred 🎯T17.3 stub blamed *buffered* `chan<bytes>` transports
("`up_out << alert` hangs even though the buffer has room"). That
diagnosis was **wrong**. A buffered transport with room never hangs here
— the buffer filter's reader is always parked at its read rendezvous, so
the write completes. The real hang is on **unbuffered** transports, in
the **mutual-teardown** shape, and it is caused by the send being a
*blocking* write rather than anything about the buffered-chan `alt`.

The fix is one line in `src/tls.cc`:

```cpp
// before (would deadlock): auto send_close_notify = [&]() {};  // no-op stub
// naive re-enable (deadlocks):  (void)(up_out << take_buffer(sendbuf));
// fix (best-effort, non-blocking):
(void)prialt(up_out << std::move(alert), csp::none);
```

## Background

A `tls::stream` (src/tls.cc, `make_stream`) runs one steady-state imp
that drives both directions via `prialt(read_r >> req, write_r >> wbuf)`.
When either plaintext consumer endpoint drops, that `prialt` returns
`which < 0` and the imp tears down: it sends a courtesy `close_notify`
alert through its ciphertext sink `up_out`, drops both upstream
endpoints, and exits.

`close_notify` is a *courtesy*: RFC 8446 §6.1 lets the peer distinguish a
clean shutdown from a truncation attack. It is not required for
correctness of this library — the peer also observes shutdown when the
ciphertext endpoints drop as the imp exits. The original no-op stub
relied entirely on that endpoint-drop signal.

## The cast of actors

Two streams wired back-to-back over two unbuffered `chan<bytes>`
transports (the in-process test shape, and any synchronous transport
with no send buffer):

| Actor | Role |
|---|---|
| **C** — client stream imp | sends ciphertext on `cs`, consumes from `sc` |
| **S** — server stream imp | sends ciphertext on `sc`, consumes from `cs` |
| transport **cs** (C→S) | C's `up_out`; its reader is S's ciphertext source |
| transport **sc** (S→C) | S's `up_out`; its reader is C's ciphertext source |

A stream consumes ciphertext from its source only while it is in its
steady-state loop. The instant it begins teardown it stops servicing
that source.

## Numbered trace — the deadlock (blocking send)

1. C finishes its work; its plaintext consumer drops; C's `prialt`
   returns `which < 0`.
2. C begins teardown. It **stops pulling** from `sc` (it will no longer
   service S's outbound ciphertext).
3. C calls `send_close_notify` → `up_out << alert`, i.e. a **blocking**
   write onto `cs`. The write parks: it needs S to pull from `cs`.
4. Concurrently, S finishes its work the same way; S begins teardown and
   **stops pulling** from `cs` (step 2's mirror).
5. S calls `send_close_notify` → blocking write onto `sc`. It parks,
   needing C to pull from `sc`.
6. Now C is parked writing to `cs` (whose reader S has abandoned), and S
   is parked writing to `sc` (whose reader C has abandoned). Neither
   write can ever rendezvous. **Mutual deadlock** — neither imp exits;
   the runtime never goes quiescent; `csp::schedule()` never returns.

### Hypothesis (confirmed)

The close_notify send is a blocking channel write, and at teardown the
only reader for that transport is the peer stream — which has, by
symmetry, also stopped reading. Two blocking writes waiting on two
abandoned readers form a cycle.

### Invariant violated

> **Teardown progress**: a stream that begins teardown eventually exits,
> regardless of the peer's teardown timing.

A blocking `up_out << alert` violates this: the imp can park at the send
point forever when the peer has stopped draining the transport.

## Why the "buffered chan" hypothesis was a red herring

The stub feared a buffered transport whose reader is "held but idle". A
buffered `chan<T>(N)` is an imp running
`alt(in >> staging, buf.empty() ? ~out : write_op_for(out, front))`
(see `chan<T>::chan` in `include/csp/csp.h`). When the buffer is not
full, its `in >> staging` arm is **registered as a waiter** on the
buffer's read endpoint. A producer doing `prialt(~up_out, up_out <<
alert)` therefore finds a ready peer in phase 1 of `prialt_begin` and the
write rendezvous immediately — no hang. This was confirmed two ways:

- A standalone reproducer (`prialt(~w, w << alert)` on a `chan<bytes>(16)`
  with the reader held by an idle imp) returns `which == 1` at maxprocs
  1/2/4 — no hang.
- Test `TlsStream---Close-notify-over-buffered-transport` passes even
  with the *blocking* send: the alert lands in a free buffer slot
  because the buffer filter is parked at its read rendezvous.

The buffered case is exactly the case that *works*. The hazard is the
*unbuffered* (synchronous, no-cushion) case, where there is no parked
reader to absorb the alert.

## The fix

Make the send best-effort and non-blocking:

```cpp
auto send_close_notify = [&]() {
    ptls_buffer_t sendbuf;
    uint8_t       sendbuf_small[64];
    ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));
    int ret = ptls_send_alert(tls.get(), &sendbuf,
                              PTLS_ALERT_LEVEL_WARNING,
                              PTLS_ALERT_CLOSE_NOTIFY);
    if (ret == 0 && sendbuf.off > 0) {
        bytes alert = take_buffer(sendbuf);
        (void)prialt(up_out << std::move(alert), csp::none);
    }
    ptls_buffer_dispose(&sendbuf);
};
```

`prialt(up_out << alert, csp::none)` tries the write once. If a reader is
ready (a real socket's kernel send buffer, or a buffered chan's parked
filter, or a peer actively pulling ciphertext) the alert is delivered.
If not, `csp::none` fires and the alert is dropped; the peer still sees
shutdown via endpoint drop. Either way the imp never parks, so the
teardown-progress invariant holds for every interleaving.

This matches reality on all three transport shapes:

| Transport | reader at send time | outcome |
|---|---|---|
| real socket | kernel send buffer | alert flushed to wire |
| buffered chan (room) | buffer filter parked on `in` | alert buffered, delivered on next pull |
| unbuffered chan, peer pulling | peer's source at rendezvous | alert delivered, decrypted as graceful close |
| unbuffered chan, peer torn down | none | alert dropped; peer sees endpoint drop |

## Evidence

- **Repro**: enabling a blocking `send_close_notify` makes
  `TlsStream---Multiple-writes-aggregate` (unbuffered, both sides finish)
  hang → SIGTERM under a timeout. The new
  `TlsStream---Mutual-close-notify-no-deadlock` makes the mutual-teardown
  shape explicit and also hangs under the blocking send.
- **Fix verification**: with the non-blocking send, all five
  `TlsStream---*` tests pass (`Handshake-and-roundtrip-loopback`,
  `Multiple-writes-aggregate`, `Consumer-drop-sends-close-notify`,
  `Mutual-close-notify-no-deadlock`,
  `Close-notify-over-buffered-transport`).
- **Buffered-is-fine**: the buffered-transport test passes even under the
  blocking send, isolating the bug to the unbuffered case.

## TLA+ verification

`formal/CloseNotifyDeadlock.tla` (fixed) and
`formal/CloseNotifyDeadlock_Bug.tla` (blocking send) model two streams
tearing down over two transports. The liveness property is:

> `BothExit == <>(\A s \in Streams : phase[s] = "exited")`

The bug spec violates it with the 3-state counter-example matching the
trace above:

```
BeginTearDown("C")  -> phase = [C |-> "sending", S |-> "run"],    pulling = [cs |-> TRUE,  sc |-> FALSE]
BeginTearDown("S")  -> phase = [C |-> "sending", S |-> "sending"], pulling = [cs |-> FALSE, sc |-> FALSE]
(stutter)           -> neither CompleteSend enabled; both wedged forever
```

The fixed spec (non-blocking teardown: send-and-exit is atomic, so no
imp ever parks at the send point) satisfies `BothExit`.

## General principle

A teardown / cleanup action on one imp must not contain a **blocking**
rendezvous that depends on a *peer* imp which is, by symmetry, also
tearing down. Best-effort cleanup signals (close_notify, goodbye frames,
final acks) should be sent non-blocking
(`prialt(sink << signal, csp::none)`) so the cleanup path always makes
progress. The peer's own teardown — endpoint drop — is the backstop the
recipient can always rely on.

## Related

- [28-streaming-http-body-handoff.md](28-streaming-http-body-handoff.md)
  — same family: a teardown/handoff made robust with a non-blocking
  `prialt` race instead of an unconditional blocking op.
- `formal/BufferedChanFilter.tla` — the buffered-chan `alt` protocol that
  the original (incorrect) hypothesis suspected.
