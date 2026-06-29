# Paper 29: `tls::conn` over `tls::stream` — Wire-Synchronous Write *(design + implemented)*

## Status: IMPLEMENTED (2026-06-29) — see [§ Implementation](#implementation)

Design paper for 🎯T17.4 — reimplement `tls::conn` as a thin wrapper
over `tls::stream`, deleting the duplicate direct-picotls integration
in `src/tls.cc` while preserving `conn`'s synchronous-on-the-wire write
semantics. A 2026-06-14 implementation attempt failed and was reverted
(see [The failed attempt](#the-failed-attempt)); this paper settled the
bridge mechanism, and **option (c)** landed on 2026-06-29. The design
below stands as written; [§ Implementation](#implementation) records
what shipped and the three places the realised mechanism had to refine
the design's prose to be correct.

## The problem

`tls::conn` and `tls::stream` are two independent picotls integrations
that do the same crypto work through different I/O models.

`conn` (`src/tls.cc:179-391`) owns an fd and drives picotls
**synchronously on the calling imp**. Its write path is the crux:

```cpp
ssize_t conn::write(const void* buf, size_t len) {        // src/tls.cc:354
    ptls_buffer_t sendbuf;
    // ... ptls_send encrypts plaintext into sendbuf ...
    flush_to_socket(impl_->fd, sendbuf);                  // src/tls.cc:366
    ptls_buffer_dispose(&sendbuf);
    return static_cast<ssize_t>(len);
}
```

`flush_to_socket` (`src/tls.cc:135-148`) loops `wait_writable` +
`::write` until every ciphertext byte has left the process. **When
`conn::write` returns, the ciphertext is on the wire.** This is the
contract every `conn` caller relies on: in
`TLS---Handshake-and-data-roundtrip` (`test/tls.test.cc:89-91`) the
server does

```cpp
c.write("pong", 4);
c.shutdown();
csp::io::close(client_fd);     // safe: "pong" already flushed
```

The `close(fd)` immediately after `write` is only safe because `write`
guarantees the bytes are gone before it returns.

`stream` (`src/tls.cc:487-653`) uses the opposite model. Its outbound
side is a `writer<bytes>` (`plaintext_out`, declared
`include/csp/tls.h:124`). A plaintext write is a channel rendezvous:

```cpp
client.plaintext_out << ping;          // test/tls_stream.test.cc:107
```

That rendezvous hands plaintext to the steady-state stream imp, which
encrypts it (`send_plain`, `src/tls.cc:575-590`) and pushes the
**ciphertext** onward via `up_out << std::move(out)` (`src/tls.cc:589`).
`up_out` is the caller-supplied ciphertext sink — over a real socket,
that sink is a `byte_writer` consumer imp
(`include/csp/part/io.h:36-47`, and the dup'd-fd variant in
`src/net.cc:52-61`):

```cpp
for (bytes chunk; in >> chunk;) {
    if (csp::io::write(wfd, chunk.data(), chunk.size()) < 0) break;
}
```

So in a stream-backed world the ciphertext crosses **two** rendezvous
and **one** independent imp before it reaches `io::write`:

```
user imp:           plaintext_out << plain      (rendezvous #1)
stream imp:         ptls_send → up_out << cipher (rendezvous #2)
byte_writer imp:    in >> cipher → io::write(fd) (the actual wire)
```

`plaintext_out <<` (rendezvous #1) completes the instant the stream
imp accepts the plaintext. At that point the ciphertext has not been
produced, let alone flushed. **There is no point in the user imp's
timeline at which "the bytes are on the wire" is observable.** The
synchronous-on-the-wire contract has no representation in the stream
API.

## The failed attempt

The 2026-06-14 rewrite made `conn::write` delegate to
`plaintext_out << buf` and returned. `TLS---Handshake-and-data-
roundtrip` failed: the server's `pong` was silently dropped. The
revert comment captures the root cause precisely (`src/tls.cc:163-177`):

> the synchronous-on-write semantics (write() returns only after bytes
> hit the wire) cannot be preserved across the spawn-and-rendezvous
> bridge that a stream wrapper would introduce — a sink imp consuming
> stream::plaintext_out's ciphertext runs concurrently with the user's
> imp, so write() returning after the plaintext rendezvous does not
> imply the ciphertext has been flushed before a subsequent close(fd).

### The race, as an actor sequence

Actors: **U** (user imp running `conn::write` then `close(fd)`), **S**
(stream steady-state imp, `src/tls.cc:607-645`), **W** (`byte_writer`
sink imp), **K** (kernel / socket).

1. U: `plaintext_out << "pong"` — rendezvous #1 with S. **Returns.**
2. U: (naive) `conn::write` returns; U proceeds to `conn::shutdown()`
   then `io::close(fd)`.
3. S: wakes from its `prialt` write branch, runs `send_plain`:
   `ptls_send` encrypts, `up_out << cipher` — rendezvous #2 with W.
4. W: `in >> cipher` completes; W calls `io::write(fd, cipher, n)`,
   which may yield at `wait_writable`.
5. K: U's `close(fd)` (step 2) and W's `io::write(fd)` (step 4) are now
   **unordered**. If `close` wins, the fd is gone (or a `shutdown`
   already sent FIN); W's `io::write` fails with `EBADF`/`EPIPE`, the
   `byte_writer` loop breaks (`include/csp/part/io.h:43`), and "pong"
   never reaches the peer.

The invariant `conn::write` must restore: **no plaintext write returns
to the user until the corresponding ciphertext has been handed to
`io::write` and that write has completed.** Steps 1–2 must not be
reorderable ahead of steps 3–4.

The same race applies to the handshake (`drive_handshake`,
`src/tls.cc:420-483`, pushes handshake ciphertext through `up_out`) and
to `close_notify` on shutdown (currently a no-op in stream —
`send_close_notify`, `src/tls.cc:602`, with the buffered-chan hang
documented at `src/tls.cc:592-601`).

## What "thin wrapper" must preserve

The acceptance criteria for 🎯T17.4 constrain the solution:

1. `conn`'s read/write/handshake/shutdown delegate to `tls::stream`.
2. `conn` keeps its public API: fd-taking constructor
   (`include/csp/tls.h:66`), byte-buffer `read`/`write`
   (`include/csp/tls.h:78,81`).
3. `conn::write` returns only after the encrypted bytes have hit the
   wire — so a subsequent user `close(fd)` cannot race in-flight
   ciphertext.
4. All `test/tls.test.cc` tests pass via the new path.
5. The original `conn` body in `src/tls.cc` is removed once verified.

Criterion 3 is the whole problem. Criteria 1–2 say the bridge must sit
*inside* `conn`, behind its existing signatures — the read path
(`conn::read` → `stream::plaintext_in`, a pull source) is already a
clean match and needs only adaptation, but the write path needs a wire-
completion signal that `stream` does not currently emit.

A structural note constrains the wiring. `conn` must build the
ciphertext source/sink pair that `make_stream` consumes
(`include/csp/tls.h:131-135`: `io::source ciphertext_in`,
`writer<bytes> ciphertext_out`). The natural construction is
`io::fd_source(fd)` for inbound and a `byte_writer`-style sink for
outbound — but **whoever owns the sink imp owns the wire-completion
signal**. That is the lever every option below pulls.

## Options

### Option (a): request-shaped `plaintext_out`

Change `tls::stream` so the outbound plaintext side is a request
channel instead of a fire-and-forget writer:

```cpp
// include/csp/tls.h
struct stream {
    io::source                       plaintext_in;
    writer<request<bytes, monostate>> plaintext_out;  // was writer<bytes>
    std::string                      negotiated_alpn;
};
```

A plaintext write becomes `plaintext_out(buf)` (the blocking
`operator()` sugar, `include/csp/csp.h:1048`): it sends the plaintext
**and a one-shot reply channel**, then blocks for the reply. The
stream imp encrypts, pushes ciphertext to `up_out`, *waits for the sink
to confirm the write*, then writes the reply — releasing the caller.

For this to mean "on the wire", the ciphertext sink must itself be
request-shaped (the stream imp's `up_out << cipher` must block until
`io::write` completes). That is a second API change cascading down: the
ciphertext sink the user passes to `make_stream` would need to be a
`writer<request<bytes, monostate>>` too, with the underlying
`byte_writer` acking after each `io::write`.

**Wire-synchrony guarantee.** Strong and end-to-end: the caller's
`plaintext_out(buf)` reply is written only after the stream imp's
`up_out(cipher)` reply, which the sink writes only after `io::write`
returns. Acks chain wire→sink→stream→user.

**Public-API impact.** Large. `plaintext_out` changes type, breaking
every existing `stream` consumer:
`test/tls_stream.test.cc:93,107,167-169` all do
`stream.plaintext_out << value`. The ciphertext sink parameter of
`make_stream` (`include/csp/tls.h:133`) also changes type, breaking
`make_connection` (`src/net.cc`) and the test harnesses
(`test/tls_stream.test.cc:83,99` pass plain `writer<bytes>` sinks).

**Complexity.** Moderate but viral: every layer that touches the
ciphertext sink must adopt the request shape, including the generic
`byte_writer` part (`include/csp/part/io.h`), which is used far beyond
TLS.

**Handshake / shutdown.** Handshake flushes (`drive_handshake`) would
also push through the request-shaped sink and naturally block until
each flush is acked — solving handshake-vs-close too. `close_notify`
on shutdown gets the same guarantee for free.

**Verdict.** Cleanest *semantics*, worst *blast radius*. It re-shapes a
public type and forces the request protocol onto an unrelated generic
combinator. Rejected unless we decide `stream`'s outbound side *should*
be demand-shaped for independent reasons (it is plausible it should —
see [Open questions](#open-questions)).

### Option (b): per-write drain protocol inside `conn`

Leave `tls::stream`'s public API untouched. Inside `conn`, own the
ciphertext sink imp and instrument it to count completed `io::write`
calls. `conn::write` pushes plaintext via `plaintext_out << buf`, then
**blocks until the sink's completed-write count advances past the
push** that carried the resulting ciphertext.

The bookkeeping is the hard part. One `plaintext_out << buf` may
produce zero, one, or several `up_out << cipher` pushes (TLS record
fragmentation; handshake produces a burst). The conn-owned sink would
need to ack *each* ciphertext chunk, and `conn::write` would need to
know *how many* chunks its plaintext produced before it can wait for
the matching number of acks — but that count lives inside the stream
imp's `send_plain`, which `conn` cannot see.

Approximations exist (e.g. "drain until the sink is idle and its input
channel is empty"), but "idle + empty" is not a clean edge in CSP: the
sink being momentarily blocked in `wait_writable` is indistinguishable
from it having finished. Getting this right means reconstructing, from
outside, the causal chain that option (a) expresses directly.

**Wire-synchrony guarantee.** Achievable but fragile — depends on a
count reconciliation across an imp boundary the count doesn't cross.

**Public-API impact.** None. This is its only real advantage.

**Complexity.** High and subtle: a counting/quiescence protocol that
duplicates information already implicit in the rendezvous chain.

**Handshake / shutdown.** The handshake runs *before* the steady-state
imp spawns (`drive_handshake` is called inline in `make_stream`,
`src/tls.cc:505`), pushing directly to `ciphertext_out`. So `conn`
would need the drain protocol to span both the inline handshake phase
*and* the steady-state phase — exactly the "bridging handshake →
steady-state acks" the revert comment flags as the hard case
(`src/tls.cc:176-177`).

**Verdict.** No API change, but it rebuilds option (a)'s guarantee out
of weaker parts. The "count pushes vs. acks" bookkeeping is precisely
the bug-prone surface CLAUDE.md's formal-verification guidance warns
about. Rejected as primary.

### Option (c, recommended): wire-synchronous ciphertext sink owned by `conn` (no `stream` API change)

The insight that collapses the problem: **`conn` does not need
`stream` to be request-shaped; it needs the *ciphertext sink* to be
wire-synchronous, and `conn` constructs that sink itself.**

`conn` already must build the `ciphertext_in` / `ciphertext_out` pair
it feeds to `make_stream`. Make the outbound side a sink that, instead
of being a free-running `byte_writer`, **acks every chunk back to
`conn` after `io::write` completes**, over a private channel `conn`
holds. Then `conn::write` becomes:

```
plaintext_out << buf            // rendezvous #1 → stream imp
drain_acks()                    // block until the sink reports the
                                // ciphertext for this write is flushed
```

Crucially this is *not* option (b)'s "count pushes you can't see".
Because TLS write framing is **synchronous within the stream imp** —
`send_plain` (`src/tls.cc:575-590`) does `ptls_send` then `up_out <<
out` *before* returning to the imp's `prialt` loop — the stream imp
does not accept the *next* plaintext write until the *current* write's
ciphertext has been handed to the sink. So `conn` can use the channel
backpressure that already exists, with no external count:

> Make the conn-owned ciphertext sink **synchronous-on-write**: it
> reads one ciphertext chunk, calls `io::write` to completion, and only
> *then* reads the next. Make the channel between the stream imp and the
> sink **unbuffered**. Then the stream imp's `up_out << cipher`
> (rendezvous #2) does not complete until the sink has *taken* the
> chunk — and if the sink takes a chunk only when ready to write it,
> we still need one more hop to know the write *finished*.

So the sink must signal completion explicitly. The minimal mechanism:
`conn` owns a private `chan<monostate> wire_ack`. The conn-owned sink
imp is:

```cpp
// Pseudocode — conn-owned ciphertext sink.
for (bytes chunk; cipher_r >> chunk;) {
    io::write_all(fd, chunk);        // loop to completion (like flush_to_socket)
    wire_ack_w << monostate{};       // "this chunk is on the wire"
}
```

and `conn::write` is:

```cpp
ssize_t conn::write(const void* buf, size_t len) {
    size_t before = sink_writes_;            // acks consumed so far
    stream_.plaintext_out << bytes(p, p+len);// hand plaintext to stream imp
    // Drain acks until the ciphertext produced by THIS write is flushed.
    drain_to_quiescence();                   // see below
    return static_cast<ssize_t>(len);
}
```

The remaining question is the same one option (b) faces: *how many*
acks correspond to one plaintext write. Option (c) answers it
structurally instead of by counting, using a **round-trip marker**:

After `plaintext_out << buf`, `conn` issues a **zero-cost sentinel
round-trip through the stream imp's own ordering**. Because the stream
imp processes its `prialt` write branch to completion (`ptls_send` +
`up_out <<`) before reading the next plaintext, and because all of one
write's ciphertext chunks are pushed *in order, before* any later
write's, `conn` can append a private control write that the stream imp
forwards as an empty/sentinel ciphertext push. When `conn` sees the
sentinel's ack, every chunk *ahead* of it — i.e. all ciphertext from
the preceding `plaintext_out << buf` — has already been flushed
(FIFO on both the unbuffered stream→sink channel and the sink's
serial `io::write` loop).

If a sentinel control path is judged too invasive, the simpler and
fully sufficient variant: **make the stream→sink channel unbuffered and
the sink serial**, and have `conn::write` block until the sink has
*drained back to empty* — observable because the sink, after writing a
chunk and posting its ack, attempts the next `cipher_r >> chunk`, which
blocks (no producer) once the burst is done. `conn::write` reads acks
until reading one more would block; with an unbuffered upstream and a
serial sink, "would block" coincides exactly with "all ciphertext for
this write is flushed", because the stream imp cannot have queued a
*later* write's ciphertext (it is parked in `prialt` waiting for
`conn`'s next plaintext, which `conn` has not sent — `conn` is
single-threaded per connection and `conn::write` is the only writer).

That last clause is the load-bearing simplification and is worth
stating as an invariant:

> **conn-serial-writes invariant**: `conn` issues at most one
> `plaintext_out << buf` at a time and does not issue the next until the
> current `conn::write` returns. Therefore, while `conn::write` drains
> acks, no *new* plaintext can be in flight, so any ciphertext the
> stream imp is still holding belongs to the current write. Draining
> until the sink blocks on an empty channel is exact.

This invariant is true for `conn` by construction (its API is a
synchronous, single-imp byte interface) and is exactly the property
the original `flush_to_socket` exploited implicitly.

**Wire-synchrony guarantee.** Strong, and proven by the invariant: when
`conn::write` returns, the serial sink has executed `io::write` to
completion for every ciphertext chunk of this write, and no later write
exists. A subsequent `close(fd)` cannot race.

**Public-API impact.** **None.** `tls::stream`'s `plaintext_out`,
`plaintext_in`, and `make_stream` signatures are untouched. The ack
channel and the wire-synchronous sink are entirely private to `conn`'s
implementation in `src/tls.cc`. `make_connection` (`src/net.cc`) and
the `stream` tests keep their fire-and-forget `writer<bytes>` sinks.

**Complexity.** Moderate, and *local*. No counting across imp
boundaries (the invariant removes the count); no change to generic
`byte_writer`; no protocol viral spread. The only new machinery is one
private `chan<monostate>` and a drain loop, both inside `conn`.

**Handshake / shutdown.** The handshake (`drive_handshake`) already
runs inline in `make_stream` and pushes through the *same*
`ciphertext_out`. Routing it through the conn-owned wire-synchronous
sink means handshake ciphertext is also flushed-to-wire by
construction — `conn::handshake()` returns only after the final
handshake flush is on the wire, matching the original
(`flush_to_socket` inside the handshake loop, `src/tls.cc:233`).
`conn::shutdown()` writes `close_notify`; routing it through the same
sink and draining gives `conn` the on-the-wire close_notify that
`stream` currently punts on (`src/tls.cc:592-602`). This is a strict
improvement: stream's documented buffered-chan hang does not arise
because `conn`'s sink is a real socket writer, not a held-but-idle
`chan<bytes>`.

**Verdict.** Recommended. It achieves option (a)'s end-to-end wire
synchrony with option (b)'s zero public-API impact, by recognising that
the wire-completion signal belongs to the sink — which `conn` owns —
not to `stream`'s public type. The serial-writes invariant turns the
"count pushes vs. acks" bookkeeping into a clean channel-emptiness
test.

## Recommendation

Adopt **option (c)**: a private, wire-synchronous, serial ciphertext
sink owned by `conn`, draining over an unbuffered stream→sink channel,
with the conn-serial-writes invariant making "drained to empty" an
exact proxy for "this write is on the wire". No change to `tls::stream`'s
public API.

Defer option (a) unless a *separate* requirement (e.g. HTTP/2 flow
control wanting per-write backpressure on plaintext) justifies making
`stream`'s outbound side demand-shaped. If that day comes, option (a)
subsumes (c) and `conn` simplifies to `plaintext_out(buf)`. Record that
linkage rather than pre-paying its cost now.

## Implementation sketch

1. **`conn::impl`** holds: the `tls::stream` (built in the constructor
   from `io::fd_source_view(fd)` inbound + the conn-owned sink
   outbound — `_view` because `conn` owns the fd lifecycle and closes
   it itself, matching today's `conn::~conn`), the private
   `reader<monostate> wire_ack`, and a small read-side leftover buffer
   (the read path returns "up to len", so `conn::read` must buffer
   excess plaintext exactly as `stream`'s `plainbuf` already does —
   `conn::read` adapts `stream::plaintext_in` with its own leftover,
   `src/tls.cc:264-274` shows the existing leftover discipline to
   replicate).

2. **conn-owned wire-synchronous sink imp** (private to `src/tls.cc`):
   reads ciphertext chunks from the stream's outbound channel over an
   **unbuffered** `chan<bytes>`, `io::write`-to-completion each (reuse
   the `flush_to_socket` loop body), posts `wire_ack << monostate{}`
   after each. Closes nothing on exit (fd owned by `conn`); on
   `io::write` error, `_throw` is unavailable on a plain channel, so
   surface the error by dropping the ack writer — `conn::write`'s drain
   observes the ack channel dying and throws `tls::error` / `csp::error`
   at the call site (preserving today's throwing behaviour at
   `src/tls.cc:145`).

3. **`conn::handshake()`** delegates to `make_stream`, which drives the
   handshake inline through the conn-owned sink; returns after the
   final flush is acked. Cancellation still works: the inline handshake
   pulls ciphertext via `io::call_source` on `fd_source_view`, which
   parks in the reactor and observes `done()` — preserving
   `TLS---Cancel-during-handshake` (`test/tls.test.cc:123-161`).
   (Note: `make_stream` builds the steady-state imp; `conn` must ensure
   the handshake's leftover ciphertext path and the steady-state path
   share the one sink. Confirm the stream imp's spawn does not race the
   sink — both are spawned by `conn` before the first `conn::write`.)

4. **`conn::write(buf, len)`**: `stream_.plaintext_out << bytes(...)`,
   then drain acks until the channel is empty per the invariant. Return
   `len`. On ack-channel death, throw.

5. **`conn::read(buf, len)`**: serve from the leftover buffer if
   non-empty; else `bytes b = stream_.plaintext_in(chunk)` (the
   blocking source call, `include/csp/tls.h` / `source.h` `operator()`),
   copy up to `len`, stash the remainder. EOF (peer close_notify or
   transport EOF) surfaces as `stream::plaintext_in`'s reply-writer
   death → return 0, matching today's `conn::read` returning 0
   (`src/tls.cc:314,327`).

6. **`conn::shutdown()`**: route `close_notify` through the same sink
   and drain (best-effort, swallow errors as today, `src/tls.cc:380-385`).

7. **Delete** the old `conn::impl` direct-picotls bodies
   (`src/tls.cc:179-391`) and the now-conn-only socket helpers if
   unused. Keep `flush_to_socket`'s loop logic (reused by the new sink).

### How each acceptance criterion is met

| Criterion | Mechanism |
|---|---|
| Delegate read/write/handshake/shutdown to `stream` | Steps 3–6: all four route through `make_stream`/`plaintext_in`/`plaintext_out`. |
| Keep public API (fd ctor, byte-buffer read/write) | `include/csp/tls.h` `conn` declarations unchanged; only `conn::impl` changes. |
| `write` returns only after ciphertext hits the wire | Step 2 + step 4 + conn-serial-writes invariant: drain-to-empty ⟺ flushed. |
| All `tls.test.cc` tests pass | Steps 3–6 preserve each path; see verification below. |
| Old `conn` impl removed | Step 7. |

### Tests that verify the wire-synchrony guarantee

- **`TLS---Handshake-and-data-roundtrip`** (`test/tls.test.cc:59`):
  the `write("pong") → shutdown → close(fd)` sequence
  (`test/tls.test.cc:89-91`) is the exact race; passing it is the
  primary proof. This is the test the 2026-06-14 attempt failed.
- **`TLS---Large-transfer`** (`test/tls.test.cc:348`): 256 KB →
  `write → shutdown → close`. Exercises multi-record fragmentation —
  one `conn::write` producing many ciphertext chunks — confirming the
  drain waits for *all* chunks, not just the first.
- **`TLS---Concurrent-connections`** (`test/tls.test.cc:266`): N
  independent conns each `write → shutdown → close`; confirms the
  per-conn private ack channel does not cross-talk.
- **`TLS---conn-move-semantics`** (`test/tls.test.cc:414`): the
  `conn::impl` (now owning a `stream` + ack channel) must remain
  movable; defaulted moves over `unique_ptr<impl>` cover this.
- **Cancellation tests** (`TLS---Cancel-during-handshake`,
  `TLS---Cancel-during-read`, `test/tls.test.cc:123,163`): confirm the
  pull-source path preserves `done()`/timeout observation.

## TLA+ obligation

Per CLAUDE.md's formal-verification convention ("when writing concurrent
decision points… write the safety invariant first… run TLC before
writing the C++"), option (c) has exactly one concurrent decision
point: **`conn::write`'s drain-loop exit** — the decision "the
ciphertext for this write is fully flushed, so it is safe to return
(and for the caller to `close(fd)`)."

Write the spec before the code:

- **Safety invariant** (`WireFlushedBeforeReturn`): whenever
  `conn::write` returns, the count of `io::write`-completed ciphertext
  chunks equals the count produced by all `plaintext_out << buf` issued
  so far. Equivalently: no ciphertext chunk is in the stream→sink
  channel or in the sink's pre-`io::write` state when `write` returns.
- **Model the actors**: user imp (serial writes), stream imp
  (`prialt` write branch: take plaintext → produce 1..k ciphertext
  chunks → push each, in order, before reading next plaintext), sink
  imp (serial: take chunk → write → ack), and the channels' buffering
  (unbuffered stream→sink). Model `close(fd)` as an action gated on
  `conn::write` having returned.
- **The bug spec** (`...Bug.tla`): a buffered stream→sink channel *or*
  a concurrent (non-serial) sink — either breaks the "drain-to-empty ⟺
  flushed" equivalence and lets `close(fd)` race a chunk still in the
  buffer. TLC should report the violation, reproducing the 2026-06-14
  drop. Pair as `TlsConnDrain.tla` (holds) + `TlsConnDrain_Bug.tla`
  (violates), each with a `.cfg`, per the `formal/` convention.

The serial-sink + unbuffered-channel design is *because* the spec
shows the buffered/concurrent variants are unsafe; the spec is the
diagnostic that scopes why the invariant holds, not post-hoc
documentation.

## Open questions

- **Sentinel vs. drain-to-empty.** The sketch above prefers
  drain-to-empty (no control-write path), justified by the
  conn-serial-writes invariant. If profiling shows the empty-channel
  probe adds a scheduling round-trip per write that matters, the
  sentinel-marker variant amortises it. Start with drain-to-empty;
  measure before adding the sentinel.

- **Does option (a) become the right answer for HTTP/2?** If 🎯T3.7's
  HTTP/2 layer wants per-write plaintext backpressure (so a slow socket
  throttles the producer), `stream`'s outbound side may want to be
  demand-shaped regardless of `conn`. If that lands, `conn` collapses to
  `plaintext_out(buf)` and this whole drain mechanism retires. Track the
  dependency; do not pre-build it.

- **fd ownership and the read source.** `conn` owns the fd and closes it
  in its destructor. The inbound side should therefore be
  `io::fd_source_view(fd)` (non-owning), so the stream's read imp does
  not close the fd out from under `conn`. Confirm the stream imp's EOF
  path (`src/tls.cc:561-567`) does not assume source-owned-fd
  semantics — it doesn't (it only drops endpoints), but verify during
  implementation.

- **Error propagation through a plain-channel sink.** A `writer<bytes>`
  sink cannot `_throw`. The sketch surfaces sink-side `io::write`
  errors by dropping the ack writer (death = error at the drain site).
  An alternative is a request-shaped *private* ack carrying
  `exception_ptr` — more faithful but heavier. Decide during
  implementation; the plain-death path matches today's coarse "throw
  `csp::error` on socket write failure" (`src/tls.cc:145`).

## Implementation

Option (c) shipped in `src/tls.cc` (the `conn` section) on 2026-06-29.
`conn::impl` holds the `tls::stream`, a depth-1 `reader<std::monostate>`
wire-ack, and a `writer<request<monostate, monostate>>` control endpoint;
`conn::handshake()` builds the transport, spawns the private sink, calls
`make_stream`, and arms the sink. All 12 `tls.test.cc`/`tls_stream.test.cc`
cases pass (the stream's public API is untouched, so the stream tests were
not edited). The drain-loop exit decision is verified by
[`formal/TlsConnDrain.tla`](../../formal/TlsConnDrain.tla) (holds) paired
with `TlsConnDrain_Bug.tla` (the reverted "return after the plaintext
rendezvous" behaviour — TLC reports `returned=1, flushed=0`, the dropped
"pong").

Three refinements were needed where the design's prose, taken literally,
would not have been correct:

1. **Per-write completion is a *blocking single ack*, not "drain to
   empty".** The [Open questions](#open-questions) preferred draining until
   the conn-owned sink blocks on an empty channel. That probe is racy at the
   *start* of a write: `conn::write` can observe the channel empty before the
   stream imp has produced the ciphertext at all. The realised code instead
   relies on the structural fact that one steady-state plaintext write
   produces *exactly one* ciphertext push (`send_plain` does one `ptls_send`
   → one `up_out <<`, regardless of record fragmentation), so `conn::write`
   hands over the plaintext and then **blocks for exactly one** wire ack.
   The conn-serial-writes invariant guarantees that ack belongs to this
   write. This "one push per write" (k=1) is the load-bearing fact the TLA+
   model encodes; a future change that fragmented a write into multiple
   pushes would have to revisit it.

2. **The sink is *armed* after the handshake; the arm doubles as the
   handshake's wire-sync barrier.** [§ Option (c) Handshake/shutdown](#options)
   said handshake ciphertext routes "through the conn-owned wire-synchronous
   sink", but if the sink acked every handshake flush, those acks would
   either deadlock an unbuffered ack channel (no one drains it while
   `make_stream` runs inline) or pile up as stale acks the first write's
   drain cannot disambiguate. The sink therefore *writes without acking*
   until `conn` sends a one-shot `ctl` request after `make_stream` returns.
   Because the sink selects ciphertext and `ctl` serially in one `prialt`,
   the arm is ordered after the last handshake flush — so its reply gives
   `conn::handshake()` a wire-synchronous completion *and* leaves the ack
   channel empty for the first write. (The ack channel is depth-1 so the
   trailing close_notify ack lands without blocking the sink during
   teardown.)

3. **Read cancellation needs a `shutdown(SHUT_RD)` wake, not just
   `done()`.** [§ Implementation sketch step 3/5](#implementation-sketch)
   assumed the inbound pull "parks in the reactor and observes `done()`".
   That holds for the *handshake* pull (the conn-owned source is spawned
   inside the handshake's cancel scope, so its `io::read` observes `done()`
   and forwards `csp::canceled` across the reply — `conn` uses a
   cancel-forwarding source variant rather than `fd_source_view`, which would
   `std::terminate` on an uncaught `canceled`). But a *steady-state* read's
   cancel scope is created after the handshake, so the long-lived source imp
   does not share it: `conn::read` observes the cancellation on its own imp
   via `prialt(done(), reply_r >> b)` and throws, but that leaves the source
   imp parked in `io::read` on the fd. `io::close` does not wake a reactor
   waiter, so `conn::read` first issues `shutdown(fd, SHUT_RD)` to surface
   EOF and let the source — and the stream imp blocked behind it — tear
   down. `conn::shutdown()` likewise drains the wire-ack until the sink exits,
   so a caller-side `close(fd)` cannot race a still-pending `io::write` under
   fd recycling.

These are refinements *within* option (c) — the recommendation to keep
`tls::stream`'s public API unchanged held. Option (a) remains the right
answer only if a separate requirement makes `stream`'s outbound side
demand-shaped (see [Open questions](#open-questions)).

## See also

- [Paper 19 — Pull-Based Sources](19-pull-based-sources.md): the
  `io::source` abstraction `conn::read` consumes via
  `stream::plaintext_in`; Stage 5 there anticipated wrapping `conn`.
- [Paper 15 — Channels as Interfaces](15-channels-as-interfaces.md):
  the `request<Req, Resp>` pattern option (a) would adopt for
  `plaintext_out`.
- [Paper 03 — Two-Phase prialt](03-two-phase-prialt.md): the
  rendezvous/`chan_op` semantics the drain protocol relies on.
- [Paper 28 — Streaming HTTP Body Handoff](28-streaming-http-body-handoff.md):
  a prior case of bridging a synchronous caller contract to a
  rendezvous chain, with its own TLA+ pair.
