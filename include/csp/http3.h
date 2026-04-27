// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HTTP/3 over QUIC server and client (🎯T3.9).
//
// Uses the same http::request and http::response types as the HTTP/1.1 and
// HTTP/2 servers. Handler code is fully protocol-agnostic: a handler that reads
// http::request and writes http::response runs unchanged across HTTP/1.1,
// HTTP/2, and HTTP/3.
//
// Layering:
//   http3::serve/get/post
//       └── nghttp3  (HTTP/3 framing, QPACK header compression)
//           └── quic::connection  (QUIC transport — 🎯T3.8 / ngtcp2)
//               └── PicoTLS      (TLS 1.3 — always required for HTTP/3)
//
// HTTP/3 requires TLS. The API is therefore only available when compiled with
// CSP_TLS=1 (the default). HTTP/3 uses UDP port 443 by convention; the serve()
// call binds a UDP socket for QUIC.
//
// Dependency contract (T3.7 / T3.8):
//   - http::request and http::response come from <csp/http.h> (T3.7 shared
//     types). The protocol-agnostic contract is: `reader<http::request>` with
//     per-request `writer<http::response>` embedded in request::respond.
//   - QUIC streams come from <csp/quic.h> (T3.8). Each HTTP/3 stream maps to
//     one quic::stream_pair. The http3 layer reads/writes bytes on those
//     streams and feeds them to nghttp3_conn for framing.
//
// See docs/papers/20-http3-design.md for the full design.

#ifdef CSP_TLS

#include <csp/csp.h>
#include <csp/http.h>

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace csp::http3 {

// --- Per-connection endpoint ---
//
// Each accepted QUIC+HTTP/3 connection yields one endpoint.
// endpoint::streams delivers one http::request per HTTP/3 stream; the
// per-request respond writer carries exactly one http::response back.

struct endpoint {
    reader<http::request> streams;  // one per HTTP/3 stream (bidirectional)
    std::string remote_addr;
};

// --- Server options ---

struct serve_options {
    // Maximum number of concurrent bidirectional HTTP/3 streams per connection.
    uint64_t max_streams = 128;

    // UDP receive buffer size in bytes (0 = OS default).
    int rcvbuf = 0;

    // PEM file paths for the server TLS certificate and private key.
    // HTTP/3 always requires TLS; self-signed certs are acceptable for testing.
    std::string cert_pem;
    std::string key_pem;
};

// --- Server ---

struct server {
    reader<endpoint> endpoints;  // one per accepted QUIC connection
    uint16_t port;               // actual bound UDP port
    std::string local_addr;
};

// Start an HTTP/3 server on the given UDP port.
// Returns a server whose endpoints reader yields one endpoint per connection.
// Dropping the reader stops accepting new connections.
//
// HTTP/3 always uses TLS 1.3 (QUIC requirement). cert_pem and key_pem must
// be paths to PEM-encoded ECDSA (secp256r1) certificate and key files, or
// empty to generate a self-signed certificate at runtime (test use only).
//
// TODO(T3.8): Implementation blocked on quic::listen() stabilisation.
// Stub compiles and links; serve() throws csp::error("http3: not yet
// implemented") at runtime.
[[nodiscard]] server serve(uint16_t port, serve_options opts = {});
[[nodiscard]] server serve(const std::string& addr, uint16_t port,
                           serve_options opts = {});

// --- Client fetch options ---

struct fetch_options {
    // Custom certificate verification callback. If null, no verification is
    // performed (acceptable for testing only).
    using verify_fn = std::function<bool(
        const char*, const std::vector<std::vector<uint8_t>>&)>;
    verify_fn verify;
};

// --- Client ---
//
// Perform an HTTP/3 request. Blocks the calling imp until the full response
// (headers + body) has been received.
//
// url must use the "https" scheme (HTTP/3 is always over TLS).
// Default port is 443. QUIC connection establishment and TLS handshake happen
// transparently.
//
// Throws csp::error on connection failure, handshake failure, or parse error.
//
// TODO(T3.8): Implementation blocked on quic::dial() stabilisation.
// Stub compiles and links; fetch() throws csp::error("http3: not yet
// implemented") at runtime.
http::response fetch(
    http::method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bytes body = {},
    fetch_options opts = {});

// Convenience wrappers.
inline http::response get(
    const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    fetch_options opts = {}) {
    return fetch(http::method::GET, url, std::move(headers), {}, opts);
}

inline http::response post(
    const std::string& url, bytes body,
    std::vector<std::pair<std::string, std::string>> headers = {},
    fetch_options opts = {}) {
    return fetch(http::method::POST, url, std::move(headers),
                 std::move(body), opts);
}

} // namespace csp::http3

#endif // CSP_TLS
