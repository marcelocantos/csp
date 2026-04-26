# TCP Networking Reference

High-level TCP primitives built on top of `csp::io`. All types live in
`namespace csp::net`.

Header: `#include "csp.h"` (via `csp/net.h`)

---

## Table of Contents

1. [csp::net::connection](#cspnetconnection) — connected socket with split I/O channels
2. [csp::net::listen](#cspnetlisten) — TCP listener that produces connections
3. [csp::net::dial](#cspnetdial) — connect to a remote host

---

## csp::net::connection

A connected socket represented as a pair of channels.

### Declaration

```cpp
struct connection {
    io::fd_t fd;                            // underlying socket (non-blocking)
    reader<std::vector<uint8_t>> input;     // bytes arriving from peer
    writer<std::vector<uint8_t>> output;    // bytes to send to peer
    std::string remote_addr;                // peer address ("host:port")
};
```

### Description

`connection` is a move-only struct. The `input` reader and `output` writer
are backed by `byte_reader` / `byte_writer` imps spawned on the socket.
Closing (dropping) either endpoint propagates to the other side:

- Dropping `output` sends a TCP FIN to the peer (half-close write).
- The `input` reader returns `false` once the peer closes or errors.
- Dropping both closes the socket entirely.

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
for (std::vector<uint8_t> chunk; conn.input >> chunk;) {
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
        std::vector<uint8_t> buf;
        while (conn.input >> buf) {
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

    for (std::vector<uint8_t> chunk; conn.input >> chunk;) {
        fwrite(chunk.data(), 1, chunk.size(), stdout);
    }
});
csp::schedule();
```

### Errors

| Condition | Effect |
|-----------|--------|
| DNS resolution failure | throws `csp::error` |
| All addresses refused / unreachable | throws `csp::error` |
| Cancel scope fires | throws `csp::canceled` |
