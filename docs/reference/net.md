# TCP Networking Reference

High-level TCP primitives built on top of `csp::io`. All types live in
`namespace csp::net`.

Header: `#include "csp.h"` (via `csp/net.h`)

---

## Table of Contents

1. [csp::net::connection](#cspnetconnection) — connected socket with split I/O channels
2. [csp::net::listen](#cspnetlisten) — TCP listener that produces connections
3. [csp::net::dial](#cspnetdial) — connect to a remote host
4. [Shared listener plumbing](#shared-listener-plumbing) — `bind_listener`, `accept_loop`, `parse_authority`

---

## csp::net::connection

A connected socket represented as a pair of channels.

### Declaration

```cpp
struct connection {
    io::fd_t fd;                            // underlying socket (non-blocking)
    io::source source;                      // pull-based reads from peer
    writer<std::vector<uint8_t>> output;    // bytes to send to peer
    std::string remote_addr;                // peer address ("host:port")
};
```

### Description

`connection` is a move-only struct. The `source` (a pull-based
`io::source`) and `output` writer are backed by reader / writer imps
spawned on the socket. Closing (dropping) either endpoint propagates to
the other side:

- Dropping `output` sends a TCP FIN to the peer (half-close write).
- Reading from `source` returns `false` (peer death) once the peer closes
  or errors.
- Dropping both closes the socket entirely.

Read by pulling sized chunks: `source(n)` blocks and returns up to `n`
bytes (throwing on error, `csp::channel_closed` on EOF), or
`io::call_source(source, n)` returns a `reader<bytes>` you can `>>` (which
returns `false` on EOF) or race in a `prialt`.

`fd` is the raw socket (already non-blocking). Prefer the channel API;
use `fd.raw()` only for platform calls that require a socket descriptor.

### Example

```cpp
auto conn = csp::net::dial("example.com", 80);

std::string req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
std::vector<uint8_t> data(req.begin(), req.end());
conn.output << data;

// Signal EOF to peer:
conn.output = {};

// Read response:
for (;;) {
    auto reply = csp::io::call_source(conn.source, 4096);
    std::vector<uint8_t> chunk;
    if (!(reply >> chunk)) break;   // EOF
    // process chunk
}
```

---

## csp::net::listen

Create a TCP listener that produces `connection` values.

### Signature

```cpp
struct listen_options {
    int backlog  = 128;
    bool reuse_addr = true;
    bool dual_stack = true;   // IPv6+IPv4 dual-stack (IPV6_V6ONLY = 0)
};

struct listener {
    reader<connection> connections;  // read to accept each connection
    uint16_t port;                   // actual bound port (useful with port 0)
    std::string local_addr;          // bound address string
};

listener listen(uint16_t port, listen_options opts = {});
listener listen(const std::string& addr, uint16_t port, listen_options opts = {});
```

### Description

`listen` binds to the given address/port and starts accepting connections
in an internal imp. Each accepted connection is sent on
`listener.connections`. The caller reads from that channel to obtain
`connection` objects; each connection is ready for immediate I/O.

Pass `port = 0` to let the OS assign an ephemeral port. The actual port is
returned in `listener.port`.

The accept loop stops when `listener.connections` is dropped (the reader
is not kept alive). The underlying socket is closed automatically.

DNS resolution for `addr` runs on the blocking pool (non-blocking for the
calling imp).

### Example

```cpp
#include "csp.h"

// Echo server on an OS-assigned port.
auto srv = csp::net::listen(0);
printf("Listening on port %u\n", srv.port);

csp::net::connection conn;
while (srv.connections >> conn) {
    csp::spawn([conn = std::move(conn)]() mutable {
        for (;;) {
            auto reply = csp::io::call_source(conn.source, 4096);
            std::vector<uint8_t> buf;
            if (!(reply >> buf)) break;   // peer closed
            conn.output << buf;
        }
    });
}
```

---

## csp::net::dial

Connect to a remote TCP host.

### Signature

```cpp
connection dial(const std::string& host, uint16_t port);
connection dial(const std::string& host, const std::string& service);
```

### Description

Resolves `host` via `csp::io::resolve` (DNS runs on the blocking pool),
then attempts each resolved address in order until one succeeds. Returns
a `connection` with a non-blocking socket and ready I/O channels.

Throws `csp::error` if all addresses fail (connection refused, timeout,
etc.). Throws `csp::canceled` if a cancellation scope fires during the
attempt.

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    auto conn = csp::net::dial("example.com", 80);

    std::string req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    std::vector<uint8_t> data(req.begin(), req.end());
    conn.output << data;
    conn.output = {};   // half-close: signal EOF

    for (;;) {
        auto reply = csp::io::call_source(conn.source, 4096);
        std::vector<uint8_t> chunk;
        if (!(reply >> chunk)) break;   // EOF
        fwrite(chunk.data(), 1, chunk.size(), stdout);
    }
});
csp::await_completion();
```

### Errors

| Condition | Effect |
|-----------|--------|
| DNS resolution failure | throws `csp::error` |
| All addresses refused / unreachable | throws `csp::error` |
| Cancel scope fires | throws `csp::canceled` |

---

## Shared listener plumbing

Protocol-neutral helpers (🎯T48) that single-source the TCP listener setup
and accept-loop machinery used by `net::listen`, `http::serve`,
`http2::serve`, and `http2::serve_tls`.

### Signature

```cpp
struct listen_result {
    io::fd_t listen_fd;
    uint16_t port = 0;          // actual bound port
    std::string local_addr;     // bound address string
    sockaddr_storage wake_addr{};  // self-connect target for wake_listener
    socklen_t wake_len = 0;
};

listen_result bind_listener(const std::string& addr, uint16_t port,
                            const listen_options& opts,
                            const std::string& resolve_err_prefix);

void wake_listener(const sockaddr_storage& addr, socklen_t len);

template <typename Endpoint, typename F>
reader<Endpoint> accept_loop(const listen_result& lr, const char* descr,
                             F on_connection);

struct authority {
    std::string host;
    uint16_t port = 0;
    std::string path = "/";
};

authority parse_authority(std::string_view url,
                          std::string_view scheme_prefix,
                          uint16_t default_port);
```

### Description

`bind_listener` resolves, binds, listens, and sets non-blocking, returning
the bound port/address plus a connectable wake address (wildcard binds are
substituted with the same-family loopback). `resolve_err_prefix` lets each
caller keep its own resolve-failure message.

`accept_loop` spawns a producer imp that accepts connections and calls
`on_connection(client_fd, remote_addr, out_copy)` per socket. A sentinel
imp watches for endpoint-reader death, sets a stop flag, and self-connects
via `wake_listener` to unblock a parked accept. `net::listen` keeps its
separate cancel-scope shutdown design and uses only `bind_listener`.

`parse_authority` parses `<scheme>host[:port][/path]` (including
`[IPv6]:port`) — the URL shape shared by `http::fetch` and `ws::connect`.
Throws `csp::error` on scheme mismatch, malformed IPv6 brackets, or an
empty host.
