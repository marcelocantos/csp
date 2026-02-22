# TLS

CSP provides cancel-aware TLS support via [mbedTLS](https://github.com/Mbed-TLS/mbedtls).
The TLS API lives in `namespace csp::tls` and is available when `CSP_TLS` is
defined at compile time. The dev build enables it by default (`CSP_TLS=1`).

## Prerequisites

TLS requires the M:N runtime (same as I/O):

```cpp
csp::init_runtime(4);
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
    ctx.load_ca(ca_pem, ca_pem_len);      // PEM, NUL-terminated

    csp::tls::conn c(ctx, fd);
    c.set_hostname("example.com");         // SNI + verification
    c.handshake();                          // cancel-aware

    c.write("GET / HTTP/1.0\r\n\r\n", 18);

    char buf[4096];
    ssize_t n;
    while ((n = c.read(buf, sizeof(buf))) > 0) {
        // process response...
    }

    c.shutdown();                           // close_notify
    close(fd);
});
```

## API

### `csp::tls::context`

A TLS configuration shared across connections. Thread-safe after construction.

```cpp
class context {
public:
    enum role { client, server };
    explicit context(role r = client);

    void load_ca(const void* pem, size_t len);
    void load_cert(const void* cert_pem, size_t cert_len,
                   const void* key_pem, size_t key_len);
};
```

- **`context(client)`** — enables peer certificate verification by default.
- **`context(server)`** — disables client certificate verification by default.
- **`load_ca`** — parse PEM CA certificate(s) for verifying the peer. The
  buffer must be NUL-terminated and `len` must include the NUL.
- **`load_cert`** — load own certificate + private key. Required for servers;
  optional for client authentication.

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
- **`set_hostname`** — sets the SNI extension and enables hostname
  verification. Call before `handshake`.
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
    int code;   // mbedTLS error code
};
```

Thrown on TLS failures (handshake rejection, verification failure, etc.).
`what()` returns a human-readable message from `mbedtls_strerror`.

## Cancellation

All blocking TLS operations (`handshake`, `read`, `write`) are cancel-aware.
They use `csp::io::wait_readable`/`wait_writable` internally, which compose
with `cancel_op()` when a cancellation scope is active.

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
ctx.load_cert(cert_pem, cert_len, key_pem, key_len);

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
own mbedTLS build. The TLS code in `csp.h`/`csp.cpp` is wrapped in
`#ifdef CSP_TLS` — without the define, it compiles to nothing.

## Design notes

- **BIO callbacks are non-blocking.** mbedTLS BIO callbacks return
  `MBEDTLS_ERR_SSL_WANT_READ`/`WANT_WRITE` instead of blocking. A C++
  retry loop calls `wait_readable`/`wait_writable` between attempts. This
  keeps C++ exceptions out of mbedTLS's C stack frames.
- **`conn` does not own the fd.** The caller manages socket lifecycle.
- **No stream parts for TLS.** `mbedtls_ssl_context` is not thread-safe
  for concurrent read+write, so separate `byte_reader`/`byte_writer` imps
  cannot safely share it. Use `conn.read()`/`conn.write()` from a single
  imp; pipeline composition happens on the plaintext side.
