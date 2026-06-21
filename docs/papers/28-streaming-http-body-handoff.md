# Streaming an HTTP Body Without Deadlocking the Connection

## Abstract

CSP's HTTP/1.1 server delivers each request to its handler as soon as the
headers are parsed, then streams the body to the handler chunk by chunk as it
arrives off the wire. This keeps memory bounded by the read chunk size rather
than the body size — a 1 GiB upload costs 4 KiB of buffer. But streaming a
body to a handler that may not read it is a deadlock waiting to happen: an HTTP
server must be free to reject a request (`401`, `413`) *before* consuming its
body. This paper describes the handoff protocol that makes streaming robust —
a `prialt` that races the body push against the handler's response — and the
TLA+ spec (`formal/HttpBodyStreamHandoff.tla`) that proves it never wedges the
connection. It is the design behind 🎯T17.5.

## 1. Why deliver before the body arrives

The original server (🎯T3.2) accumulated the whole request body in
`state.body` via llhttp's `on_body` callback, then delivered the request — body
in hand — only at `on_message_complete`. Simple, and robust: the body is a
self-contained `bytes`, so a handler can read it, ignore it, or reject the
request without any coordination. The cost is memory: a streaming upload is
fully buffered before the handler sees a single byte.

The fix is to deliver the request at `on_headers_complete` and expose the body
as a `reader<bytes>` (`request::body_stream`) whose chunks are produced as
`on_body` fires. Memory drops to one read chunk. The handler reads the stream
incrementally, or calls `request::drain()` to collect it, exactly as before.

The catch is the same property that made the buffered design easy. A handler
is entitled to respond without reading the body:

```cpp
if (req.content_length() > limit)
    req.respond << http::response{413, {}, too_big};   // never touches the body
```

In the buffered design this is free. In a streaming design it is a trap. If the
connection orchestrator pushes body chunks with a plain blocking send:

```cpp
for (chunk : body) body_ch.w << chunk;   // blocks until the handler reads
resp_ch.r >> resp;                       // only reached after the whole body
```

then the orchestrator blocks forever on `body_ch.w << chunk` — nobody is
reading `body_stream` — while the handler blocks forever on `req.respond <<
resp` — nobody is receiving on `resp_ch` until the orchestrator reaches the
line below the loop, which it never will. The connection is wedged. Worse, a
keep-alive connection that *did* survive would be desynchronised: the next
request would be parsed starting in the middle of the abandoned body.

## 2. The race

The orchestrator must do two things at once during the body phase: feed the
body to the handler, and listen for a response that may arrive at any moment.
These are exactly the two arms of a `prialt`. Each body chunk is handed over
not with a blocking send but with a select that races the push against the
response:

```cpp
switch (prialt(body_ch.w << std::move(state.body_stage),
               resp_ch.r >> resp)) {
case 0:  break;                        // chunk delivered, keep streaming
case 1:  resp_received = true; break;  // handler responded early
case ~0: deliver_body  = false; break; // handler dropped body_stream
case ~1: goto done;                    // handler abandoned the response
}
```

CSP's `prialt` returns the index of the arm that fired, or its bitwise
complement (`~0`, `~1`) when that arm fired because the *peer endpoint died*
— a closed channel is a first-class, selectable event here, not an error code
checked after the fact (see paper 15, *Channels as Interfaces*). The four cases
are the four things a handler can do with a chunk it is being offered:

- **`0` — it reads.** Normal streaming. Continue to the next chunk.
- **`1` — it responds.** An early `401`/`413`. Take the response now; stop
  offering body chunks.
- **`~0` — it drops `body_stream`.** The handler is done with the body but
  hasn't responded yet. Stop offering chunks; wait for the response in the
  phase below.
- **`~1` — it drops `respond`.** The handler died without answering. The
  connection is unsalvageable; close it.

Crucially the body push happens *outside* `llhttp_execute`. The parser's
`on_body` callback only stages this read's bytes into `state.body_stage` (a
buffer bounded by the read chunk size); the actual handoff — the part that can
block — happens back in the orchestrator loop where a `prialt` can guard it.
A callback cannot select; a loop can.

## 3. Draining keeps keep-alive honest

Once the handler has responded early (case `1`) or dropped the stream (case
`~0`), the orchestrator stops *delivering* the body but keeps *parsing* it: it
reads the remaining body bytes off the wire and discards them, advancing
llhttp to `on_message_complete`. Only then does it write the response and
resume the parser for the next keep-alive request.

This is the invariant that makes pipelining safe: **the response is written
only after the entire body has been consumed off the wire.** Skip the drain and
the next request is parsed starting inside the previous body — a request
smuggling bug, not merely a correctness nuisance. The orchestrator reaches its
response phase by exactly one path (`wire empty ∧ nothing staged`), so the
invariant holds by construction.

## 4. Proving it doesn't wedge

The handoff is a small concurrent protocol with a non-deterministic adversary
(the handler), which is precisely the shape TLA+ is good at.
`formal/HttpBodyStreamHandoff.tla` models the orchestrator, a body of
`NumChunks` chunks, and a handler that non-deterministically reads, drops the
stream, or responds at any point. It checks two properties:

- **`KeepAliveSync`** (invariant): `orch ∈ {respond, done} ⇒ wire = 0` — the
  response phase is reachable only with the body fully drained.
- **`Terminates`** (liveness): `<>(orch = done ∧ handler = done)` — whatever
  the handler does, the connection finishes.

TLC explores the full state space (59 states) and both hold. The companion
`HttpBodyStreamHandoff_Bug.tla` models the naive blocking-push design of §1;
TLC produces a counterexample in which a handler commits to `reject`, the
orchestrator stages a chunk, and the system stutters forever — the deadlock,
made concrete. The pair is the standard CSP convention: a fixed spec and the
bug it rules out, each runnable with `./formal/tlc HttpBodyStreamHandoff` and
`./formal/tlc HttpBodyStreamHandoff_Bug`.

## 5. What it buys

The streaming handler now sees the request the instant its headers land, reads
the body at its own pace with one chunk of buffering, and is free to reject
early without thought for the plumbing. The orchestrator absorbs all the
coordination: race the push against the response, drain whatever the handler
left behind, keep the wire aligned for the next request. The whole protocol is
one `prialt` and a drain loop — small enough to read top to bottom, and proven
not to deadlock.

## See also

- Paper 15, *Channels as Interfaces* — endpoint death as a selectable event,
  the mechanism behind the `~0`/`~1` cases.
- Paper 19, *Pull-Based Sources* — the `io::source` the orchestrator reads
  through (🎯T17.1).
- `formal/HttpBodyStreamHandoff.tla` / `_Bug.tla` — the model checked here.
- `src/http.cc` `handle_connection` — the implementation.
