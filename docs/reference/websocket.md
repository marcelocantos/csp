# WebSocket Reference

Channel-native WebSocket support for both server-side upgrades and client
connections. Lives in `namespace csp::ws`.

Header: `#include "csp.h"`

WebSocket support requires the optional `dist/csp_ws.cpp` drop-in (or
`src/ws.cc` for an in-tree build), plus the vendored wslay library
(`vendor/github.com/tatsuhiro-t/wslay/lib/`). Server upgrades also need the
HTTP drop-in and llhttp.

---

## Table of Contents

1. [Types](#types)
   - [csp::ws::opcode](#cspwsopcode)
   - [csp::ws::message](#cspwsmessage)
   - [csp::ws::conn](#cspwsconn)
2. [Server-side upgrade](#cspwsupgrade)
3. [Client-side connect](#cspwsconnect)
4. [Lifecycle and close handshake](#lifecycle-and-close-handshake)

---

## Types

### csp::ws::opcode

```cpp
enum class opcode : uint8_t {
    text   = 0x1,
    binary = 0x2,
    close  = 0x8,
    ping   = 0x9,
    pong   = 0xA,
};
```

Only `text` and `binary` appear in user-visible messages. `close`, `ping`,
and `pong` are handled automatically by the library.

---

### csp::ws::message

```cpp
struct message {
    opcode op   = opcode::binary;  // text or binary
    bytes  data;                   // payload bytes
};
```

Each value exchanged via `conn.recv` or `conn.send` is a `message`. The
`op` field distinguishes text frames (UTF-8 payload) from binary frames
(arbitrary bytes). The library does not validate UTF-8 content for text
frames; this is the caller's responsibility.

---

### csp::ws::conn

```cpp
struct conn {
    reader<message> recv;   // inbound messages (text and binary only)
    writer<message> send;   // outbound messages
    void close() const;    // abort without waiting for a Close echo
};
```

Returned by `upgrade` and `connect`. Both endpoints are independent:
dropping `send` initiates a Close handshake (see below); dropping `recv`
signals the reader imp to stop forwarding inbound data.

`close()` is idempotent and asynchronous. It cancels socket readiness and
channel waits in both I/O imps, shuts down the socket, and releases its
descriptor only after both imps finish. Observe endpoint death to wait for
those imps. Dropping the endpoints continues to use the normal Close
handshake; use `close()` to enforce an application deadline against a peer
that does not cooperate.

### csp::ws::options

```cpp
struct options {
    size_t max_message_size = 0;
};
```

The limit applies to the assembled inbound data message across all
fragments. Zero preserves the unlimited default. An oversized advertised
frame or accumulated fragmented message aborts the connection before its
payload is appended. Control frames remain limited to 125 bytes by the
WebSocket framing rules. The receive path has no message queue: its
unbuffered channel holds at most one assembled message, plus the current
frame and wslay's fixed parser buffer. With a finite limit, each message
and frame buffer is bounded by that limit (with vector capacity overhead).
This does not impose a receive deadline or an outbound size
limit; applications should use `close()` on their own deadline.

---

## csp::ws::upgrade

Performs the HTTP→WebSocket upgrade handshake on a server-side request.

### Signature

```cpp
conn upgrade(http::request& req);
conn upgrade(http::request& req, options opts);
```

### Description

Must be called from a handler that has received an HTTP request with
`Upgrade: websocket`. Validates the required headers (`Upgrade`,
`Connection`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version: 13`), sends a
`101 Switching Protocols` response, and hijacks the raw socket from the
HTTP connection loop.

On success, returns a `conn` with live `recv` and `send` endpoints.

On error (missing or malformed upgrade headers), sends a `400 Bad Request`
response and throws `csp::error`. The HTTP connection loop continues
normally after the throw.

### Transition rules ([syntax](transition-rules.md))

```
upgrade(req) ─┤headers valid├──➤ send 101; hijack fd; return conn
upgrade(req) ─┤headers invalid├─➤ send 400; throw csp::error
```

### Example

```cpp
#include "csp.h"

auto srv = csp::http::serve(8080);
csp::http::endpoint ep;
while (srv.endpoints >> ep) {
    csp::spawn([ep = std::move(ep)] {
        csp::http::request req;
        while (ep.requests >> req) {
            if (req.header("Upgrade") == "websocket") {
                auto conn = csp::ws::upgrade(req);
                csp::ws::message msg;
                while (conn.recv >> msg) {
                    conn.send << std::move(msg);  // echo
                }
            } else {
                req.respond << csp::http::response{200, {}, {}};
            }
        }
    });
}
```

---

## csp::ws::connect

Connects to a WebSocket server and performs the opening handshake.

### Signature

```cpp
conn connect(const std::string& url);
conn connect(const std::string& url, options opts);
```

### Parameters

| Parameter | Description |
|-----------|-------------|
| `url` | `ws://host[:port]/path` URL. Only the `ws://` scheme is supported (no `wss://` in this release). Default port is 80. |

### Description

Resolves the host, establishes a TCP connection, sends the HTTP/1.1
Upgrade request, and validates the server's `101` response including the
`Sec-WebSocket-Accept` header.

On success, returns a `conn` with live `recv` and `send` endpoints.

Throws `csp::error` on DNS failure, connection failure, or invalid
server response.

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    auto conn = csp::ws::connect("ws://example.com/chat");

    std::string text = "Hello, WebSocket!";
    csp::ws::message out;
    out.op   = csp::ws::opcode::text;
    out.data = csp::bytes(text.begin(), text.end());
    conn.send << std::move(out);

    csp::ws::message in;
    if (conn.recv >> in) {
        // process reply
    }
});
csp::await_completion();
```

---

## Lifecycle and close handshake

WebSocket connections follow a close handshake per RFC 6455 §7. The library
implements this transparently via channel lifecycle (BLO pattern):

```
┌──────────────────────────────────────────────────────────┐
│  drop conn.send                                          │
│       │                                                  │
│       ▼                                                  │
│  ws_writer sends Close frame ──────────► peer ws_reader  │
│                                               │          │
│                               peer ws_reader echoes Close│
│                                               │          │
│  ws_reader receives echo ◄────────────────────┘          │
│       │                                                  │
│  ws_reader exits (conn.recv closes)                      │
└──────────────────────────────────────────────────────────┘
```

**Initiating close:** Drop `conn.send`. The writer imp detects the dead
channel, sends a Close frame, and exits. When the peer echoes the Close
frame, the reader imp exits and `conn.recv` closes.

**Receiving close:** When the peer initiates close, the reader imp receives
the Close frame, forwards an echo request to the writer imp, and exits.
`conn.recv` closes first; `conn.send` closes shortly after the echo is sent.

**Aborting:** Call `conn.close()` when an application deadline expires or
an inbound message is invalid. This does not wait for a WebSocket Close
echo and may discard an in-progress send. For example, an ACK-only server
can use `ws::upgrade(req, {.max_message_size = 20})` and race its receive
against `after(5s)`, then call `close()` if the timeout wins.

**Ping/pong:** Incoming Ping frames are answered automatically with Pong.
Pong frames are silently discarded. Applications do not see Ping or Pong
in the `recv` channel.

**Concurrency safety:** The reader imp only reads; the writer imp only
writes. Pong and close-echo frames pass through an unbuffered internal
control channel, ensuring serialised writes. A third lifetime imp owns
their cancellation scope and the socket. It joins both I/O imps before
releasing the descriptor, including after cancellation, so a resumed writer
cannot access a descriptor already reused by another connection.
