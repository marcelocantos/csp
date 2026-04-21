# Pull-Based Sources: Composable Sized Reads for CSP

## Status: DESIGN

Pre-implementation design paper for 🎯T17 — composable pull-based
sized-read abstraction in `csp::io`. No code yet; this paper exists to
settle the shape of the abstraction before any layer is touched.

## The problem

CSP's byte streams are push-based. `part::io::byte_reader` chooses a
chunk size, loops on `io::read(fd, buf, n)`, and pushes whatever the
kernel returns onto a `reader<bytes>`. The consumer has no voice in
how many bytes per message; it gets what the producer happens to
deliver.

That is fine for line-oriented protocols where `part::split_lines`
can reassemble frames downstream, and fine for raw byte pipes where
size doesn't matter. It breaks for every real network protocol.

Three concrete pain points in the current tree:

**`part::fixed_frames`** (`include/csp/part/io.h:84-111`) maintains an
internal accumulator, appending from incoming chunks byte-by-byte
until `frame_size` bytes have arrived. This is buffer-and-split
dressed up as a combinator. If the network happens to deliver neat
frame-sized chunks, the buffer stays empty; if it delivers 1 byte
then 4095 bytes then 4096 bytes, it splices. Every consumer that
needs a specific byte count reimplements this pattern.

**`http::fetch`** (on the `http-client-and-bullseye-rename` branch)
dumps whatever `conn.input >> chunk` returns into `llhttp_execute`
and relies on llhttp's internal state machine to absorb framing. It
gets away with it because llhttp is tolerant. The cost: the entire
response body is accumulated in `resp_parse_state::body` before
`fetch` returns. There is no streaming. A 1 GB download is a 1 GB
allocation.

**`http::serve`'s `handle_connection`** (`src/http.cc`) bypasses
`net::connection` entirely, reading directly from the fd in 4 KB
chunks. The comment blames cancel-scope inheritance on the accept
loop: the channel abstraction was unusable for its own primary
consumer, and the workaround is a raw `io::read` loop. This is a
direct indictment of the push model — when the architect of the
layer had to skip it, the layer is the problem.

The pattern is consistent: anywhere a consumer needs N specific
bytes, it must buffer-and-split, either manually or through a
combinator that hides the accumulator. The abstraction is at the
wrong end.

## The gap

The raw syscall `io::read(fd, buf, n)` is consumer-controlled. The
caller asks for up to N bytes; the kernel returns what it has. This
is the primitive at the bottom of every non-blocking I/O story,
including CSP's.

The channel layer throws that property away. `reader<bytes>` carries
producer-shaped chunks — it replaces consumer-controlled pull with
producer-controlled push. From that point upward, every consumer in
the pipeline is operating in a weaker model than the syscall
underneath them.

A separate gap: `csp::tls::conn` (`include/csp/tls.h:72-75`) has
imperative `read`/`write` methods. It is not wrapped in channels at
all. This is not a design choice — it is the push model's admission
that TLS has no clean composition point. A ciphertext fragment does
not align with a plaintext read. A push-based TLS source has to
choose a plaintext chunk size at the source, not at the consumer,
and gets it wrong for every consumer that wanted a different size.
The imperative fallback is a workaround, and it means TLS does not
participate in the channel graph — no prialt, no endpoint-death
composition, no topology surgery.

Both gaps close if the consumer can say how many bytes it wants.

## The solution is already in the toolbox

Paper 15 established that channel interfaces for demand traffic are
built from `request<Req, Resp>`: a request value carries its own
one-shot reply channel, so N concurrent callers share one request
endpoint without their responses interleaving. This is the canonical
pull pattern in CSP.

A sized read is a specialisation:

```cpp
using read_request = request<size_t, bytes>;
using source       = writer<read_request>;
```

A source is a writer into which consumers submit `read_request`
values. Each request carries the desired byte count and a one-shot
reply channel. The source imp reads the request, fulfills up to N
bytes, writes the result into the reply, and the reply closes. No
new protocol, no new type, no new machinery — everything comes from
existing `request<>` semantics.

Three axes of outcome — success, EOF, error — and three distinct
structural channels carry them:

- **Success** rides the reply value itself (`bytes`).
- **EOF** rides reply-writer death: the source imp exits without
  writing, the reply's writer endpoint drops, the consumer's `>>`
  observes peer death and returns false (or the corresponding
  prialt branch fires on `~reply_reader`).
- **Errors** ride a per-transfer exception channel — the source
  calls `req.reply._throw(std::current_exception())`
  instead of `req.reply << buf`, and the consumer's `>>` rethrows
  at the call site. The channel remains live; a subsequent
  operation is free to succeed. Semantically this mirrors C++
  function calls: the type signature doesn't enumerate what can
  throw, but exceptions flow through regardless.

The exception-channel mechanism is a CSP-level primitive, not a
T17-local invention. It deserves its own target (see [Target
dependency](#target-dependency) below). This paper is written as if
that primitive exists — adopting it throughout simplifies every
layered error-handling story, not just T17's.

Consumer side, using the existing `operator()` sugar for request
writers:

```cpp
source s = /* ... */;
bytes b = s(4096);     // throws on error; throws on EOF-mid-read
                       // (via channel_closed rethrown from the reply reader)
```

For EOF-without-exception, use the non-blocking form and test:

```cpp
auto r = call(s, 4096);     // returns reader<bytes>
bytes b;
if (r >> b) {               // throws if source delivered an exception;
    // got bytes                false if reply-writer died cleanly (EOF)
} else {
    // EOF
}
```

In prialt, a firing branch that received an exception rethrows at
the call site. Cancellation and death behave unchanged:

```cpp
prialt(
    r >> b,      // fires on value or exception (rethrows);
                 // also fires as 'false' on clean death (EOF)
    ~other,      // fires on other's endpoint death
    done()       // fires on cancellation
);
```

The source producer is equally routine:

```cpp
spawn([req_r = std::move(ch.r), fd] {
    read_request req;
    while (req_r >> req) {
        try {
            bytes buf(req.value);
            ssize_t n = io::read(fd, buf.data(), buf.size());
            if (n < 0) throw csp::errno_error("read", errno);
            if (n == 0) return;                 // EOF: exit, reply-writer dies
            buf.resize(static_cast<size_t>(n));
            req.reply << std::move(buf);
        } catch (...) {
            req.reply._throw(std::current_exception());
            return;                             // terminal: exit after throwing
        }
    }
});
```

Errors are *terminal* in this imp: the source throws across one
reply, then exits, so the consumer's next request observes
request-channel death. A source variant that wants to remain open
after an error simply omits the `return` — one exception per
affected transfer, next request is fresh.

This is a `reader<request<size_t, bytes>>` served imperatively. It
is also, trivially, a combinator — and the combinator catalog gets
three new entries: `fd_source(fd)`, `tls_source(tls_conn)`,
`http_body_source(upstream, content_length)`.

## Layered composition

The payoff is that each layer is a function from `source` to
`source`. The consumer at the top always sees the same interface;
the layers below arrange themselves.

### TCP source (bottom)

Owns an fd; serves requests one-at-a-time via `io::read`. Partial
reads are fine — the contract is "up to N bytes", not "exactly N".

```
  request{n, reply}
  ──────────────▶   ┌───────────────┐
                    │   fd_source   │ ── io::read(fd, _, n) ──▶ kernel
  bytes (<= n)      │     (imp)     │
  ◀──────────────   └───────────────┘
```

EOF: `io::read` returns 0. The imp exits *without* writing a reply
to the current request. The reply-writer drops; the consumer's
reader observes its death; the consumer's pending read fails
cleanly, signalling "stream ended during this request". A next
request, if attempted, fails earlier because the request-reader
endpoint has died. Death is the protocol — and because the reply's
writer-death is observable separately from the request channel's
death, EOF mid-request and EOF between-requests are both signalled
structurally without a sentinel value.

### TLS source (middle)

Owns a downstream `source` and a TLS context. Holds an internal
plaintext buffer because TLS records don't align with consumer
reads. On request:

```
  request{n, reply}
  ──────────────▶   ┌───────────────┐
                    │  tls_source   │ ── request{m, ...} ──▶ fd_source
  bytes (<= n)      │     (imp)     │                          │
  ◀──────────────   └───────────────┘ ◀────────── bytes ───────┘
                           │
                           ▼
                    [ plaintext buf ]
```

If the plaintext buffer already has ≥ 1 byte, fulfil from it (up to
N). Otherwise, pull ciphertext from downstream with a request of the
source's choosing (typically 4–16 KB — large enough to hold one TLS
record without syscall thrash), decrypt through picotls into the
buffer, and fulfil.

Errors from picotls or the downstream source propagate via the
channel-exception mechanism. The TLS imp's read from the downstream
source rethrows (the downstream source threw across the reply); the
TLS imp catches, wraps through `std::throw_with_nested` (or
constructs a typed `tls_error` with the nested IO exception
attached), and throws across its own upstream reply. The layered
error chain stays intact across the composition without any
variant, `expected`, or explicit error type appearing in the source
interface.

### HTTP body source (top)

Owns an upstream `source` (TCP or TLS) and a content-length or
chunked-decoder. Caps each fulfilment at `remaining` bytes. When
`remaining` hits zero, exits cleanly; the consumer learns the body
ended.

For `Transfer-Encoding: chunked`, the imp runs a tiny chunked
decoder, requesting chunk-size lines from upstream, then requesting
the declared chunk size, then consuming the trailing CRLF. The
decoder is a coroutine-shaped imp; the consumer above it sees a
plain source with framing invisible.

## Semantics

**Partial reads.** A fulfilment of `m` bytes with `m < n` is a
normal success. Consumer decides whether to re-request. This matches
`io::read` and standard stream semantics; anything stricter pushes
blocking policy into the source, which is the wrong layer to decide.

**EOF.** The source imp exits after its final fulfilment; the
request-reader closes; the consumer's next request or prialt
observes endpoint death. No sentinel value, no special bytes — CSP's
existing "death is observable" machinery carries the signal.

**Errors.** Death is structural — it carries no payload. That works
for EOF (the signal *is* "no more data") but breaks for errors,
where the consumer needs to know *what* went wrong. Errors ride the
channel-exception mechanism: the source calls
`req.reply._throw(e)` instead of `req.reply << buf`, and
the consumer's `>>` rethrows `e` at the call site. No variant type,
no `expected` wrapper, no explicit error appearance in the interface
— the mechanism mirrors how C++ function calls propagate exceptions
without declaring them in the signature.

The source chooses whether to exit after throwing (terminal — the
default for I/O and TLS failures, where the underlying resource is
gone) or continue serving requests (survivable — e.g., a validator
that rejects malformed requests without tearing down). Terminal is
the common case and the examples in this paper all use it.

Layered sources translate errors at their abstraction level. A TLS
source that catches the rethrown IO exception from its downstream
source wraps through `std::throw_with_nested` (or constructs a typed
`tls_error` with the nested exception attached) and throws across
its own upstream reply. An HTTP body source does the same at its
layer, producing `http_error{"body read failed", nested: ...}`.
Each consumer sees errors in the vocabulary of its layer, with the
full chain preserved for diagnostic tooling.

**Cancellation is not an error.** If the consumer drops the reply
reader before the source writes, the source's `req.reply << ...`
observes a dead reader and discards the bytes (or the error). The
source keeps serving subsequent requests; no error reply is sent,
no death propagates. Cancellation terminates one request, not the
source.

Mid-read cancellation (kernel blocked in `io::read`) is a separate
problem — it requires either a cancel fd, a signal-driven interrupt,
or splitting each syscall into a reactor-polled chunk. This paper
defers it; the existing reactor already handles I/O readiness, so a
source imp that pulls through the reactor is implicitly cancellable
at poll boundaries.

**Pipelining.** A source imp that serves one request at a time
serialises. For parallel fulfilment (e.g., range requests on HTTP/2),
spawn one source imp per logical stream. This is idiomatic CSP —
concurrency is structural, not flag-driven.

## Migration

Stage 1 adds the `source` alias and three factories (`io::fd_source`,
`tls::source`, `http::body_source`) alongside existing code. Nothing
breaks. Tests demonstrate the three layers individually.

Stage 2 migrates `net::connection` internally: the old
`input: reader<bytes>` becomes a consumer over an internal
`fd_source`, keeping the push-shaped interface as a compatibility
adapter. Existing byte_reader users unchanged; new code uses the
source directly.

Stage 3 rewrites `http::fetch` to stream the body through a
`body_source` into a caller-supplied `writer<bytes>` (default: an
in-memory accumulator for backward compatibility). A 1 GB response
is no longer a 1 GB allocation.

Stage 4 rewrites `http::serve`'s `handle_connection` on top of a
`source`, deleting the direct-fd bypass. The cancel-scope issue that
forced the bypass was about the accept loop's scope leaking into the
connection-handler I/O imps. A source imp spawned inside the handler
owns its own channels and does not inherit the accept-loop cancel
guard, so the bypass becomes unnecessary.

Stage 5 wraps `tls::conn` in a source — either by replacing the
imperative API or by offering both. This is the payoff: TLS joins
the channel graph and composes with prialt, fuse, splice.

Each stage is independently mergeable and testable.

## Target dependency

T17's design as written above depends on a CSP-level primitive that
does not yet exist: **the ability to deliver an exception across a
channel in place of a value.** This deserves its own target, written
with roughly this acceptance shape:

- `writer<T>::_throw(std::exception_ptr)` delivers an
  exception on the next rendezvous instead of a value.
- A reader's `r >> val` observing an exception-delivery rethrows at
  the call site. The channel remains live after the exception — a
  subsequent operation on the same channel is free to succeed,
  mirroring that a C++ function can throw on one call and return
  normally on the next.
- A prialt branch `r >> val` fires when an exception is delivered;
  the exception propagates out of prialt at the branch's call site.
  Death branches (`~r`, `done()`) remain structural and distinct.
- The two-phase prialt protocol (paper 03) is extended to carry an
  exception unit alongside the typed value; `chan_op`'s RAII
  destructor still runs to completion so `w << val` as a statement
  can throw without violating invariants.
- Buffered channels carry exceptions in-order with values; a
  consumer reaches them by reading past any preceding values.

Without this primitive, T17 falls back to
`std::expected<bytes, std::exception_ptr>` as the reply type, and
every layered source spells its error-unwrapping at each seam. The
paper above documents the clean version; the fallback is a
mechanical transformation if the primitive is deferred. Build order:
ship the channel-exception target first, then T17 on top.

See also: `request<Req, Resp>` in paper 15 never had an error story
either. The primitive improves every request/response interaction in
CSP, not just T17's.

## Open questions

- **Should `bytes` be `std::vector<uint8_t>` or a view type?** The
  source imp already owns a buffer for the syscall; moving a vector
  across the channel is one pointer swap. A view type (span over a
  shared buffer) saves the allocation on the receive side at the
  cost of lifetime complexity. Start with `vector`; measure before
  optimising.

- **Default chunk size at the bottom of the stack.** Inside
  `fd_source`, the consumer sets `n`, but `io::read` may return
  less. Should `fd_source` loop internally to fill N, or return
  immediately on first success? Return immediately, matching
  `read(2)` semantics. Consumers that want exactly-N can wrap with a
  small `exact_source` combinator.

- **Zero-request.** A consumer asking for 0 bytes is degenerate.
  The source throws `std::invalid_argument` across the reply —
  zero-length reads almost always indicate a bug in the caller, and
  the channel-exception mechanism surfaces it at the call site
  without tearing down the source.

- **Backwards compatibility on `net::connection`.** Adding a
  `source` member alongside `reader<bytes> input` is cheap but
  doubles the lifecycle surface. A migration that deprecates `input`
  after a stable release is cleaner than perpetual coexistence.

- **WebSocket and HTTP/2 framing.** Both rely on tight frame-size
  control over an underlying TLS source. The source abstraction
  should make them trivial; confirm by sketching 🎯T3.5 (WebSocket)
  and 🎯T3.7 (HTTP/2) before committing to the Stage-5 migration.

## See also

- [Paper 15 — Channels as Interfaces](15-channels-as-interfaces.md):
  `request<Req, Resp>` as the canonical demand pattern.
- [Paper 02 — Channel Lifecycle](02-channel-lifecycle.md): endpoint
  death as the signalling primitive that makes EOF-via-death work.
- [Paper 07 — Topology Surgery](07-channel-fuse-split.md): splice/fuse
  as the composition operators on which the migration stages rely.
