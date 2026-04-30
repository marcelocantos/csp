# Paper 20: HTTP/3 over QUIC — Design and Integration Contract

**Status**: Scaffolded. Implementation blocked on 🎯T3.8 (QUIC transport).

**Related targets**: 🎯T3.9, 🎯T3.8, 🎯T3.7

---

## Overview

HTTP/3 is the third major version of HTTP. Unlike HTTP/1.1 (TCP) and HTTP/2
(TCP + HPACK), HTTP/3 runs over QUIC — a UDP-based transport that provides
stream multiplexing, congestion control, and TLS 1.3 in a single layer.

The CSP layering for HTTP/3 is:

```
csp::http3::serve / get / post
    └── nghttp3 (HTTP/3 framing, QPACK header compression)
        └── csp::quic::connection (QUIC transport — ngtcp2)
            └── PicoTLS (TLS 1.3)
                └── UDP socket (reactor-integrated)
```

The key design goal is **protocol-agnostic handler code**: an application that
reads `http::request` and writes `http::response` works unchanged across
HTTP/1.1, HTTP/2, and HTTP/3. All protocol differences are hidden below the
channel layer.

---

## Dependency Contracts

This section documents what T3.9 assumes about its dependencies so that
integration after T3.7 and T3.8 complete is straightforward.

### T3.7 (HTTP/2 shared types)

T3.7 established the protocol-agnostic request/response contract:

```cpp
// include/csp/http.h

struct request {
    http::method method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
    writer<response> respond;   // write exactly one response
    // ...
};

struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
};
```

T3.9 uses these types verbatim. No changes to `http.h` are required.

The server-side endpoint contract is also the same:

```cpp
struct endpoint {
    reader<http::request> streams;  // one per request (HTTP/3 stream)
    std::string remote_addr;
};
```

### T3.8 (QUIC transport)

T3.9 needs the following from `csp::quic` (declared in `include/csp/quic.h`):

**Types used:**

```cpp
// Bidirectional stream — wraps one HTTP/3 request stream.
struct quic::stream_pair {
    reader<std::vector<uint8_t>> input;   // bytes from peer
    writer<std::vector<uint8_t>> output;  // bytes to peer
};

// One accepted QUIC connection.
struct quic::connection {
    stream_pair open_stream();               // client side
    reader<stream_pair> incoming_streams;    // server side
    std::string remote_addr;
};

// Server listener.
struct quic::listener {
    reader<quic::connection> connections;
    uint16_t port;
    std::string local_addr;
};
```

**Functions used:**

- `quic::listen(port, opts)` — bind UDP, start accepting QUIC connections.
- `quic::dial(host, port, opts)` — connect to a QUIC server.

**Potential conflicts to resolve:**

1. **ALPN**: The QUIC listener's TLS context needs to advertise "h3" via ALPN.
   The current T3.8 `listen_options` struct has `cert_pem` / `key_pem` but no
   ALPN field. T3.9 may need to add an `alpn` field or derive it from the
   calling layer.

2. **Stream ID exposure**: nghttp3 needs to see QUIC stream IDs to distinguish
   control streams (uni) from request streams (bidi). The current
   `stream_pair` doesn't expose the stream ID. T3.9 will need either:
   - A `stream_id` field on `stream_pair`, or
   - A parallel `incoming_streams_with_id` channel that delivers
     `std::pair<int64_t, stream_pair>`.

   **Recommendation**: add `int64_t stream_id` to `quic::stream_pair`.

3. **Unidirectional streams**: HTTP/3 requires three unidirectional streams
   (control, QPACK encoder, QPACK decoder) in each direction. The current T3.8
   API only exposes bidirectional `stream_pair`. T3.9 needs either:
   - Separate `open_uni_stream()` / `incoming_uni_streams` on `connection`, or
   - A `direction` field indicating bidi vs. uni on `stream_pair`.

   **Recommendation**: add `stream_pair open_uni_stream()` and
   `reader<stream_pair> incoming_uni_streams` to `quic::connection`.

---

## Implementation Plan

### Server side

```
quic::listen(port, {cert_pem, key_pem, alpn="h3"})
    → reader<quic::connection>

For each connection:
    spawn connection_imp(conn, endpoint_writer)
    
connection_imp:
    nghttp3_conn_server_new(callbacks, settings)
    bind control stream  (open_uni_stream × 1)
    bind QPACK streams   (open_uni_stream × 2)
    
    Loop:
        alt {
            conn.incoming_streams >> sp:
                if sp.stream_id is uni:
                    feed input bytes → nghttp3_conn_read_stream
                    (nghttp3 handles internally — control/QPACK bookkeeping)
                else:
                    spawn stream_imp(sp, nghttp3_conn, request_writer)
            conn.incoming_uni_streams >> sp:
                spawn uni_reader_imp(sp, nghttp3_conn)
        }

stream_imp:
    Feed bytes from sp.input → nghttp3_conn_read_stream
        → fires on_header(name, value): accumulate into request
        → fires on_data(data, len):     accumulate body
        → fires on_end_stream:          deliver request to handler
    
    Handler writes http::response to req.respond:
        nghttp3_conn_submit_response(conn, stream_id, nva, nvlen, data)
        nghttp3_conn_writev_stream → bytes → sp.output
```

### Client side

```
quic::dial(host, port, {alpn="h3", verify=...})
    → quic::connection

nghttp3_conn_client_new(callbacks, settings)
bind control stream    (conn.open_stream — bidi used as uni on client)
bind QPACK enc/dec     (two more open_stream calls)

open request stream:
    conn.open_stream() → sp
    nghttp3_conn_submit_request(conn, sp.stream_id, nva, nvlen, data)
    
Loop:
    nghttp3_conn_writev_stream → bytes → sp.output
    sp.input bytes → nghttp3_conn_read_stream
        → on_header / on_data / on_end_stream callbacks
    exit when response complete
```

---

## nghttp3 API Notes

Key functions from nghttp3 v1.15 (verified against the vendored source):

| Function | Purpose |
|---|---|
| `nghttp3_conn_server_new` | Create server-side HTTP/3 session |
| `nghttp3_conn_client_new` | Create client-side HTTP/3 session |
| `nghttp3_conn_bind_control_stream` | Bind the outgoing control stream |
| `nghttp3_conn_bind_qpack_streams` | Bind QPACK encoder/decoder streams |
| `nghttp3_conn_read_stream` | Feed received bytes into the session |
| `nghttp3_conn_writev_stream` | Get the next bytes to send |
| `nghttp3_conn_submit_request` | Queue a client request |
| `nghttp3_conn_submit_response` | Queue a server response |
| `nghttp3_conn_block_stream` | Apply backpressure to a stream |
| `nghttp3_conn_resume_stream` | Release backpressure |

Callbacks (set in `nghttp3_callbacks`):

| Callback | Fires when |
|---|---|
| `recv_header` | A response/request header (name, value) is decoded |
| `recv_data` | A chunk of body data arrives |
| `end_stream` | All headers and data for a stream have been received |
| `begin_headers` | The header block for a new stream begins |
| `stream_close` | A stream is fully closed |
| `send_stop_sending` | The session wants to reset a stream |
| `reset_stream` | A stream has been reset by the peer |

---

## Protocol-agnostic Handler Design

The central design goal is that application handlers are unaware of the HTTP
version in use. This is achieved by hiding the protocol below the channel layer:

```cpp
// A handler that works across HTTP/1.1, HTTP/2, and HTTP/3:
void handle(reader<http::request> requests) {
    for (auto req : requests) {
        auto resp = process(req);
        req.respond << std::move(resp);
    }
}

// HTTP/1.1:
auto srv1 = http::serve(8080);
for (auto ep : srv1.endpoints)
    spawn(handle, ep.requests);

// HTTP/2:
auto srv2 = http2::serve(8080);
for (auto ep : srv2.endpoints)
    spawn(handle, ep.streams);

// HTTP/3 (once T3.8 is complete):
auto srv3 = http3::serve(443, {.cert_pem="cert.pem", .key_pem="key.pem"});
for (auto ep : srv3.endpoints)
    spawn(handle, ep.streams);
```

The protocol differences manifest only in the entry point call; the handler
function `handle` is identical. A more sophisticated dispatcher could listen on
all three simultaneously.

---

## Anti-patterns to Avoid

1. **Yielding inside C++ catch blocks.** The imp may resume on a different OS
   thread; `__cxa_eh_globals` is thread-local and will be wrong.

2. **Synchronous nghttp3 callbacks** that write to CSP channels directly. The
   nghttp3 callbacks fire inside `nghttp3_conn_read_stream`, which is called
   from the stream_imp's imp context. Writing to a channel inside a callback is
   fine as long as the callback doesn't hold nghttp3 internal locks. The nghttp3
   API is single-threaded (one session per imp) so there are no nghttp3 locks;
   the only lock risk is within CSP's own channel rendezvous, which is safe to
   call from any imp context.

3. **Sharing nghttp3_conn across imps.** Each QUIC connection gets its own
   `nghttp3_conn` instance, and that instance must be accessed only from the
   connection_imp (or its child stream_imps with explicit locking). The simplest
   design is to make the connection_imp own the session and use a shared_ptr
   with a mutex for cross-imp callbacks.

4. **Ignoring QPACK streams.** HTTP/3 connections always have three
   unidirectional streams for control and QPACK encoder/decoder. Failing to
   consume these streams causes the peer to stall waiting for flow-control
   credit.
