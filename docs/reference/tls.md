# TLS Reference

Cancel-aware TLS 1.3 via PicoTLS (minicrypto backend). Available when
`CSP_TLS` is defined.

All types live in `namespace csp::tls`. Header: `#include "csp.h"`.

---

## Table of Contents

1. [csp::tls::error](#csptlserror) -- TLS exception type
2. [csp::tls::verify_fn](#csptlsverify_fn) -- Certificate verification callback
3. [csp::tls::context](#csptlscontext) -- TLS configuration (shared across connections)
4. [csp::tls::conn](#csptlsconn) -- TLS session on a socket

---

## csp::tls::error

Exception thrown on TLS failures. Wraps a PicoTLS error code with a
human-readable message.

### Signature

```cpp
struct error : csp::error {
    int code;       // PicoTLS error code
    error(int code);
    // what() returns "tls: <description>"
};
```

---

## csp::tls::verify_fn

Callback type for custom certificate verification.

```cpp
using verify_fn =
    std::function<bool(const char* server_name,
                       const std::vector<std::vector<uint8_t>>& certs)>;
```

Receives the server name (from SNI) and the DER-encoded certificate
chain. Return `true` to accept, `false` to reject. If no verify callback
is set, no certificate verification is performed.

---

## csp::tls::context

TLS configuration: certificate chain, private key, and verification
settings. A single context can be shared across many connections.

### Signature

```cpp
class context {
public:
    enum role { client, server };
    explicit context(role r = client);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;

    void load_cert(const char* cert_pem_path);
    void load_key(const char* key_pem_path);
    void set_verify(verify_fn fn);
};
```

### Methods

| Method | Description |
|---|---|
| `context(role)` | Create a context. |
| `load_cert(path)` | Load certificate chain from a PEM file. |
| `load_key(path)` | Load private key from a PKCS#8 PEM file. Must be secp256r1 (minicrypto limitation). Required for servers. |
| `set_verify(fn)` | Set a custom certificate verification callback. Without this, no verification is performed. |

### Notes

- `context` uses pimpl (`std::unique_ptr<impl>`) to hide PicoTLS types.
- Internally holds: `ptls_context_t` (crypto config), sign certificate
  state, and the optional verify bridge.
- The minicrypto backend has no X.509 chain validator. Use `set_verify`
  with a custom callback for certificate validation.
- Only secp256r1 keys are supported (minicrypto limitation).
- TLS 1.3 only — TLS 1.2 connections will fail.

---

## csp::tls::conn

A TLS session on an existing non-blocking, connected socket. Does not
own or close the fd.

### Signature

```cpp
class conn {
public:
    conn(context& ctx, int fd);
    ~conn();
    conn(conn&&) noexcept;
    conn& operator=(conn&&) noexcept;

    void set_hostname(const std::string& hostname);
    void handshake();
    ssize_t read(void* buf, size_t len);
    ssize_t write(const void* buf, size_t len);
    void shutdown();
    int fd() const;
};
```

### Methods

| Method | Description |
|---|---|
| `conn(ctx, fd)` | Create a TLS session. `fd` must be connected and non-blocking. Suppresses SIGPIPE on the fd. |
| `set_hostname(h)` | Set SNI extension. Call before `handshake`. |
| `handshake()` | Perform TLS handshake. Cancel-aware. Throws `tls::error` on failure, `csp::canceled`/`timed_out` on cancellation. |
| `read(buf, len)` | Read up to `len` bytes. Returns bytes read, 0 on clean shutdown (`close_notify`). Cancel-aware. |
| `write(buf, len)` | Write all `len` bytes (retries partial writes). Cancel-aware. |
| `shutdown()` | Send `close_notify`. Errors are ignored (peer may have already closed). |
| `fd()` | Return the underlying file descriptor. |

### Cancel awareness

All blocking methods (`handshake`, `read`, `write`) use
`csp::io::wait_readable` / `wait_writable` internally. When a cancellation
scope is active, these compose the fd readiness signal with `done()` in
a `prialt`, so cancellation fires immediately.

### I/O model

PicoTLS operates on buffers, not sockets. The `conn` implementation
bridges between PicoTLS's buffer-based API and the socket:

1. Call PicoTLS (`ptls_handshake` / `ptls_send` / `ptls_receive`) with
   input data.
2. Flush any output bytes to the socket via `wait_writable` + `write`.
3. When PicoTLS needs more input, read from the socket via
   `wait_readable` + `read` and feed it back.

This design means PicoTLS never touches the socket directly — no
internal mutexes, no threading issues. Safe under M:N fiber migration.
