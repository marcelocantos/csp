# HTTP/1.1 Reference

Channel-native HTTP/1.1 server and client. All types live in
`namespace csp::http`.

Header: `#include <csp/http.h>`

**Note**: `http.cc` is not included in the `dist/` amalgamation because it
depends on the vendored llhttp parser (`vendor/github.com/nodejs/llhttp/`).
Compile `src/http.cc` alongside the dist files and add
`vendor/github.com/nodejs/llhttp/include` to your include path.

---

## Table of Contents

1. [csp::http::method](#csphttpmethod) — HTTP method enum
2. [csp::http::response](#csphttpresponse) — server response / client result
3. [csp::http::request](#csphttprequest) — parsed HTTP request
4. [csp::http::endpoint](#csphttpendpoint) — per-connection context
5. [csp::http::server](#csphttpserver) — server handle returned by `serve`
6. [csp::http::serve](#csphttpserve) — start an HTTP/1.1 server
7. [csp::http::fetch / get / post](#csphttpfetch-get-post) — HTTP/1.1 client

---

## csp::http::method

```cpp
enum class method {
    GET, HEAD, POST, PUT, DELETE_, PATCH, OPTIONS, CONNECT, TRACE,
};

const char* method_name(method m);
```

`DELETE_` uses a trailing underscore to avoid clashing with the POSIX macro
`DELETE`. `method_name` returns the canonical uppercase ASCII name (e.g.
`"DELETE"` — no underscore).

---

## csp::http::response

```cpp
struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
};
```

Used both as the value written to `request::respond` (server side) and as
the return value of `fetch` / `get` / `post` (client side).

If no `Content-Length` header is included in `headers`, the server
automatically appends one based on `body.size()`.

---

## csp::http::request

```cpp
struct request {
    http::method method = method::GET;
    std::string url;
    uint8_t version_major = 1;
    uint8_t version_minor = 1;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;                 // accumulated body; empty until drain() is called
    bool keep_alive = true;

    // Streaming body: chunks as they arrive from the network.
    // Closes when the body is complete (EOF).  For no-body requests the
    // channel is already closed when the request is delivered.
    reader<bytes> body_stream;

    // Write exactly one response for this request.
    writer<response> respond;

    // Convenience: find first header value by case-insensitive name.
    // Returns empty string if not found.
    std::string header(const std::string& name) const;

    // Content-Length, or -1 if absent.
    int64_t content_length() const;

    // Drain body_stream into body.  After this call, body holds the
    // complete request body and body_stream is closed.  Idempotent:
    // calling drain() when body_stream is already closed is a no-op.
    const bytes& drain();
};
```

### Body access

Two patterns are supported:

**Pattern 1 — `drain()` then `body`** (simple, fully-buffered):

```cpp
req.drain();                       // blocks until body is complete
auto& b = req.body;                // complete body bytes
```

**Pattern 2 — read `body_stream` directly** (streaming interface):

```cpp
bytes chunk;
while (req.body_stream >> chunk) {
    // process chunk
}
```

The current implementation delivers the body as a single chunk via
`body_stream`, so there is no difference in latency between the two
patterns. True chunk-by-chunk streaming (delivering body bytes as they
arrive from the network, before the full body is received) is planned for
a future release (🎯T17).

### Response protocol

Write exactly one `response` to `req.respond`, then let it drop:

```cpp
req.respond << http::response{200, {{"Content-Type", "text/plain"}},
                               bytes(body.begin(), body.end())};
```

After the `<<` completes, the response is serialised and written to the
socket. The connection's keep-alive behaviour is determined by the
`keep_alive` field (set from the request's `Connection:` header).

---

## csp::http::endpoint

```cpp
struct endpoint {
    reader<request> requests;       // one per HTTP request on this connection
    std::string remote_addr;        // peer address, e.g. "127.0.0.1:54321"
};
```

One `endpoint` is produced per accepted TCP connection. Multiple requests
may arrive on the same connection when HTTP keep-alive is active (HTTP/1.1
default). The `requests` channel closes when the connection is terminated
(peer closed, keep-alive limit reached, or parse error).

---

## csp::http::server

```cpp
struct server {
    reader<endpoint> endpoints;     // one per accepted connection
    uint16_t port;                  // actual bound port
    std::string local_addr;         // bound address string, e.g. "[::]:8080"
};
```

Dropping `endpoints` stops the accept loop and closes the listening socket.

---

## csp::http::serve

```cpp
struct serve_options {
    net::listen_options listen = {};  // backlog, reuse_addr, dual_stack
    size_t read_chunk_size = 4096;
};

server serve(uint16_t port, serve_options opts = {});
server serve(const std::string& addr, uint16_t port, serve_options opts = {});
```

Starts an HTTP/1.1 server. Returns immediately with a `server` struct
whose `endpoints` reader yields one `endpoint` per accepted connection.

Pass `port = 0` for an OS-assigned ephemeral port. The actual port is
available in `server.port`.

The server binds to `::` (all interfaces, dual-stack IPv4+IPv6) by default.
Pass an explicit `addr` to bind to a specific interface.

### Lifecycle

```
serve() ──► accept loop imp ──► connection handler imps
                │
                ▼ (endpoints dropped)
            sentinel self-connects to unblock accept
                │
                ▼
            accept loop exits, listen socket closed
```

Connection handler imps are independent of the accept loop's cancel scope.
They live until the connection is closed or the handler drops `respond`.

### Example

```cpp
#include <csp/http.h>

csp::spawn([] {
    auto srv = csp::http::serve(8080);
    printf("HTTP server on port %u\n", srv.port);

    csp::http::endpoint ep;
    while (srv.endpoints >> ep) {
        csp::spawn([ep = std::move(ep)]() mutable {
            csp::http::request req;
            while (ep.requests >> req) {
                req.drain();  // buffer the body (no-op for GET)
                std::string body = "Hello, " + req.url + "!";
                req.respond << csp::http::response{
                    200,
                    {{"Content-Type", "text/plain"}},
                    csp::bytes(body.begin(), body.end())};
            }
        });
    }
});
csp::await_completion();
```

---

## csp::http::fetch / get / post

```cpp
struct fetch_options {
    size_t read_chunk_size = 4096;
};

// General request.
response fetch(
    method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bytes body = {},
    fetch_options opts = {});

// Convenience wrappers.
response get(const std::string& url,
             std::vector<std::pair<std::string, std::string>> headers = {},
             fetch_options opts = {});

response post(const std::string& url, bytes body,
              std::vector<std::pair<std::string, std::string>> headers = {},
              fetch_options opts = {});
```

Blocks the calling imp until the full response (headers + body) has been
received. The returned `response` has `status`, `headers`, and `body`
populated.

**URL format**: `http://host[:port]/path`. Only the `http` scheme is
supported; use `csp::tls` for HTTPS. The default port is 80.

The client automatically adds:
- `Host:` derived from the URL authority
- `Content-Length:` when `body` is non-empty (and not already present)
- `Connection: close` (unless a `Connection:` header is provided)

### Errors

| Condition | Effect |
|-----------|--------|
| DNS resolution failure | throws `csp::error` |
| TCP connection failure | throws `csp::error` |
| HTTP parse error | throws `csp::error` |
| Cancel scope fires | throws `csp::canceled` |

### Example

```cpp
#include <csp/http.h>

csp::spawn([] {
    // Simple GET
    auto resp = csp::http::get("http://example.com/api/data");
    printf("status: %d\n", resp.status);

    // POST with JSON body
    std::string payload = R"({"key":"value"})";
    auto resp2 = csp::http::post(
        "http://example.com/api",
        csp::bytes(payload.begin(), payload.end()),
        {{"Content-Type", "application/json"},
         {"Authorization", "Bearer tok123"}});
    printf("created: %d\n", resp2.status);
});
csp::await_completion();
```

---

## See Also

- [net.md](net.md) — raw TCP via `net::listen` / `net::dial`
- [http2.md](http2.md) — HTTP/2 server via nghttp2
- [tls.md](tls.md) — TLS 1.3 for HTTPS
- [io.md](io.md) — non-blocking I/O primitives
