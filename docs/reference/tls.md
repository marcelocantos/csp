# TLS Reference

Cancel-aware TLS via mbedTLS. Available when `CSP_TLS` is defined.

All types live in `namespace csp::tls`. Header: `#include "csp/tls.h"`.

---

## Table of Contents

1. [error](#error) -- TLS exception type
2. [context](#context) -- TLS configuration (shared across connections)
3. [conn](#conn) -- TLS session on a socket

---

## error

Exception thrown on TLS failures. Wraps an mbedTLS error code with a
human-readable message.

### Signature

```cpp
struct error : csp::error {
    int code;       // mbedTLS error code
    error(int code);
    // what() returns "tls: <mbedtls_strerror output>"
};
```

---

## context

TLS configuration: certificate chain, private key, CA trust store, and
protocol settings. Thread-safe after construction. A single context can
be shared across many connections.

### Signature

```cpp
class context {
public:
    enum role { client, server };
    explicit context(role r = client);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;

    void load_ca(const void* pem, size_t len);
    void load_cert(const void* cert_pem, size_t cert_len,
                   const void* key_pem, size_t key_len);
};
```

### Methods

| Method | Description |
|---|---|
| `context(role)` | Create a context. `client` enables peer verification; `server` disables it. |
| `load_ca(pem, len)` | Parse PEM CA certificate(s) for peer verification. Buffer must be NUL-terminated; `len` includes the NUL. |
| `load_cert(cert, cert_len, key, key_len)` | Load own certificate + private key. Required for servers. Both PEM, NUL-terminated. |

### Notes

- `context` uses pimpl (`std::unique_ptr<impl>`) to hide mbedTLS types.
- Internally holds: `ssl_config`, `entropy_context`, `ctr_drbg_context`,
  `x509_crt` (CA chain + own cert), `pk_context` (own key).
- Entropy and DRBG are protected by `MBEDTLS_THREADING_PTHREAD` for M:N
  safety.

---

## conn

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
| `set_hostname(h)` | Set SNI extension and enable hostname verification. Call before `handshake`. |
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

### BIO callbacks

mbedTLS BIO callbacks are non-blocking: they return
`MBEDTLS_ERR_SSL_WANT_READ` / `WANT_WRITE` on `EAGAIN`. A C++ retry helper
loops the mbedTLS operation, calling `wait_readable` / `wait_writable` between
attempts. Both `WANT_READ` and `WANT_WRITE` are checked in every call to
handle TLS renegotiation transparently.
