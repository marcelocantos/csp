// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// HTTP/3 over QUIC — 🎯T3.9
//
// This file is a scaffolded skeleton. The public API (serve, get, post) is
// declared in include/csp/http3.h and documented in
// docs/papers/20-http3-design.md.
//
// Implementation status: stub. All entry points throw csp::error("http3: not
// yet implemented") at runtime. The build is clean and all types compile.
//
// Blocked on:
//   - 🎯T3.8  (quic::listen / quic::dial stabilisation)
//   - The real stream-pump loop requires a working quic::connection that
//     delivers bidirectional quic::stream_pair values.
//
// Integration plan (see docs/papers/20-http3-design.md §Implementation):
//   1. For each accepted quic::connection, spawn a connection_imp.
//   2. connection_imp creates an nghttp3_conn (server side).
//   3. For each QUIC stream arriving on connection.incoming_streams:
//      - If stream_id == 0: control stream (nghttp3 consumes directly).
//      - If stream_id is a request stream: spawn stream_imp.
//   4. stream_imp feeds bytes from quic::stream_pair::input to
//      nghttp3_conn_read_stream, which fires on_header / on_data callbacks.
//   5. Callbacks build http::request, create a per-request response channel,
//      and deliver the request to the endpoint::streams channel.
//   6. Handler writes http::response to respond; stream_imp serialises it via
//      nghttp3_conn_submit_response and nghttp3_conn_writev_stream, then
//      writes the encoded bytes to quic::stream_pair::output.
//
// nghttp3 API contract assumptions (verify against nghttp3 v1.15):
//   - nghttp3_conn_client_new / nghttp3_conn_server_new take a callbacks
//     struct and a settings struct.
//   - nghttp3_conn_bind_control_stream / bind_qpack_streams set up the
//     unidirectional HTTP/3 control and QPACK encoder/decoder streams.
//   - nghttp3_conn_read_stream(conn, stream_id, data, len, fin) processes
//     received bytes.
//   - nghttp3_conn_writev_stream(conn, &stream_id, &fin, vec, veccnt) returns
//     the next chunk to send.
//   - nghttp3_conn_submit_response(conn, stream_id, nva, nvlen, data)
//     queues a response.
//
// ngtcp2 stream_id mapping:
//   - Client-initiated bidi streams: 0, 4, 8, ... (stream_id & 0x03 == 0)
//   - Server-initiated bidi streams: 1, 5, 9, ... (stream_id & 0x03 == 1)
//   - Client-initiated uni streams:  2, 6, 10, ...
//   - Server-initiated uni streams:  3, 7, 11, ...
//   HTTP/3 uses client-initiated bidi streams for requests and three
//   server-initiated uni streams for control / QPACK encoder / QPACK decoder.

#include <csp/http3.h>

#ifdef CSP_TLS

#include <csp/csp.h>

extern "C" {
#include <nghttp3/nghttp3.h>
}

#include <stdexcept>
#include <string>

namespace csp::http3 {

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

server serve(uint16_t /*port*/, serve_options /*opts*/) {
    // TODO(T3.8): Wire to quic::listen() once that stabilises.
    // Implementation outline: see file-level comment above.
    throw csp::error("http3::serve: not yet implemented (blocked on T3.8 QUIC transport)");
}

server serve(const std::string& /*addr*/, uint16_t port, serve_options opts) {
    return serve(port, std::move(opts));
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

http::response fetch(
    http::method /*m*/,
    const std::string& /*url*/,
    std::vector<std::pair<std::string, std::string>> /*headers*/,
    bytes /*body*/,
    fetch_options /*opts*/) {
    // TODO(T3.8): Wire to quic::dial() once that stabilises.
    // Implementation outline:
    //   1. Parse url (scheme must be "https", extract host+port+path).
    //   2. quic::dial(host, port) — get a quic::connection.
    //   3. Create nghttp3_conn client side.
    //   4. Bind control and QPACK streams (open_stream() × 3).
    //   5. Open a request stream (open_stream()).
    //   6. nghttp3_conn_submit_request with :method, :path, :authority, ...
    //   7. Pump writev_stream → quic stream output.
    //   8. Pump quic stream input → nghttp3_conn_read_stream.
    //   9. Collect on_header / on_data callbacks → build http::response.
    throw csp::error("http3::fetch: not yet implemented (blocked on T3.8 QUIC transport)");
}

} // namespace csp::http3

#endif // CSP_TLS
