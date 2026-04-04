# TLS

CSP provides cancel-aware TLS 1.3 support via
[PicoTLS](https://github.com/h2o/picotls) with the minicrypto backend (no
OpenSSL dependency). The TLS API lives in `namespace csp::tls` and is available
when `CSP_TLS` is defined at compile time. The dev build enables it by default
(`CSP_TLS=1`).

## Prerequisites

TLS requires the M:N runtime (same as I/O). The runtime auto-initializes with
hardware concurrency by default, so no explicit setup is needed. To override:

```cpp
csp::set_maxprocs(4);   // optional: set before first spawn/schedule
```

## Quick example

```cpp
#include "csp.h"

// Connect to a TLS server and send/receive data.
csp::spawn([&] {
    // Create a non-blocking TCP socket and connect.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    csp::io::set_nonblock(fd);
    // ... connect to server ...

    // Set up TLS.
    csp::tls::context ctx(csp::tls::context::client);
    ctx.set_verify(my_verify_fn);            // custom cert verification

    csp::tls::conn c(ctx, fd);
    c.set_hostname("example.com");           // SNI
    c.handshake();                           // cancel-aware

    c.write("GET / HTTP/1.0\r\n\r\n", 18);

    char buf[4096];
    ssize_t n;
    while ((n = c.read(buf, sizeof(buf))) > 0) {
        // process response...
    }

    c.shutdown();                            // close_notify
    close(fd);
});
```

## API

### `csp::tls::context`

A TLS configuration shared across connections.

```cpp
class context {
public:
    enum role { client, server };
    explicit context(role r = client);

    void load_cert(const char* cert_pem_path);
    void load_key(const char* key_pem_path);
    void set_verify(verify_fn fn);
};
```

- **`context(role)`** — creates a TLS context.
- **`load_cert`** — load certificate chain from a PEM file.
- **`load_key`** — load private key from a PKCS#8 PEM file. Must be secp256r1
  (minicrypto limitation). Required for servers.
- **`set_verify`** — set a custom certificate verification callback. Without
  this, no certificate verification is performed.

### `csp::tls::verify_fn`

```cpp
using verify_fn =
    std::function<bool(const char* server_name,
                       const std::vector<std::vector<uint8_t>>& certs)>;
```

Receives the server name (from SNI) and the DER-encoded certificate chain.
Return `true` to accept, `false` to reject.

### `csp::tls::conn`

A TLS session on an existing non-blocking socket.

```cpp
class conn {
public:
    conn(context& ctx, int fd);

    void set_hostname(const std::string& hostname);
    void handshake();
    ssize_t read(void* buf, size_t len);
    ssize_t write(const void* buf, size_t len);
    void shutdown();
    int fd() const;
};
```

- **`conn(ctx, fd)`** — the fd must be connected and non-blocking. `conn`
  does not own or close the fd.
- **`set_hostname`** — sets the SNI extension. Call before `handshake`.
- **`handshake`** — performs the TLS handshake. Cancel-aware: if a
  cancellation scope fires, the handshake throws `csp::canceled` (or
  `csp::timed_out` for deadline cancellations).
- **`read`** — reads up to `len` bytes. Returns bytes read, 0 on clean
  shutdown (close\_notify received).
- **`write`** — writes all `len` bytes (retries partial writes internally).
- **`shutdown`** — sends close\_notify to the peer.

### `csp::tls::error`

```cpp
struct error : csp::error {
    int code;   // PicoTLS error code
};
```

Thrown on TLS failures (handshake rejection, verification failure, etc.).
`what()` returns a human-readable description.

## Cancellation

All blocking TLS operations (`handshake`, `read`, `write`) are cancel-aware.
They use `csp::io::wait_readable`/`wait_writable` internally, which compose
with `done()` when a cancellation scope is active.

```cpp
auto cancel = csp::cancellation(std::chrono::seconds(5));

try {
    c.handshake();
    // ... read/write ...
} catch (const csp::timed_out&) {
    // deadline expired during TLS I/O
}
```

## Server example

```cpp
csp::tls::context ctx(csp::tls::context::server);
ctx.load_cert("certs/server.crt");
ctx.load_key("certs/server.key");

// Accept loop.
csp::spawn([&, listen_fd] {
    for (;;) {
        int fd = csp::io::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) break;
        csp::io::set_nonblock(fd);

        csp::spawn([&ctx, fd] {
            csp::tls::conn c(ctx, fd);
            c.handshake();
            // ... serve client ...
            c.shutdown();
            close(fd);
        });
    }
});
```

## Distribution

For dist users: `#define CSP_TLS` before including `csp.h`, then link your
own PicoTLS build (minicrypto backend). The TLS code in `csp.h`/`csp.cpp` is
wrapped in `#ifdef CSP_TLS` — without the define, it compiles to nothing.

## Design notes

- **Buffer-based I/O model.** PicoTLS operates on buffers, not sockets.
  The `conn` implementation bridges between PicoTLS and the socket: it calls
  PicoTLS functions with input data, flushes output to the socket via
  `wait_writable`, and reads more input via `wait_readable`. PicoTLS never
  touches the socket directly.
- **Fiber-safe.** PicoTLS has zero internal mutexes. The only thread-local
  state is a PRNG (seeded from `/dev/urandom`), which is harmless under M:N
  fiber migration — a migrated imp simply uses the target thread's PRNG.
- **`conn` does not own the fd.** The caller manages socket lifecycle.
- **No stream parts for TLS.** A TLS connection is not safe for concurrent
  read+write, so separate `byte_reader`/`byte_writer` imps cannot safely
  share it. Use `conn.read()`/`conn.write()` from a single imp; pipeline
  composition happens on the plaintext side.
- **TLS 1.3 only.** The minicrypto backend does not support TLS 1.2.
- **No built-in X.509 verification.** The minicrypto backend has no
  certificate chain validator. Use `set_verify` for custom verification.
- **secp256r1 keys only.** The minicrypto backend only supports ECDSA P-256
  private keys.
