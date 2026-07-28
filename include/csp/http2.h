// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <csp/csp.h>
#include <csp/http.h>

#include <cstdint>
#include <string>

// HTTP/2 server and stream types.
//
// Uses the same http::request and http::response types as the HTTP/1.1 server.
// Each HTTP/2 stream appears as a reader<http::request> + the per-request
// writer<http::response> embedded in http::request::respond.
//
// HPACK header compression is handled transparently by nghttp2.
// Flow control maps naturally to channel backpressure: the imp that reads
// from data providers blocks until nghttp2 grants flow-control credits.
//
// TLS + ALPN: when compiled with CSP_TLS=1, serve_tls() performs a TLS
// handshake with ALPN negotiation (h2 vs http/1.1) and returns an HTTP/2
// server. Plain-text serve() is also provided for h2c (HTTP/2 cleartext,
// useful for testing and trusted networks).

namespace csp::http2 {

// --- Stream endpoint ---
//
// Each HTTP/2 stream is delivered as an http::request whose respond writer
// accepts exactly one http::response.  This is the same interface as the
// HTTP/1.1 server.

// --- Server options ---

struct serve_options {
    int backlog = 128;
    bool reuse_addr = true;
    bool dual_stack = true;
    size_t read_chunk_size = 16384;
};

// --- Per-connection endpoint ---
//
// Each accepted connection yields one endpoint.  endpoint::streams delivers
// one request per HTTP/2 stream.

struct endpoint {
    reader<http::request> streams;  // one per HTTP/2 stream
    std::string remote_addr;
};

// --- Server ---

struct server {
    reader<endpoint> endpoints;  // one per accepted connection
    uint16_t port;               // actual bound port
    std::string local_addr;
};

// Start an HTTP/2 cleartext (h2c) server on the given port.
// Returns a server whose endpoints reader yields one endpoint per connection.
// Dropping the reader stops accepting.
server serve(uint16_t port, serve_options opts = {});
server serve(const std::string& addr, uint16_t port, serve_options opts = {});

#ifdef CSP_TLS

// Start an HTTP/2 server with TLS + ALPN negotiation.
// cert_pem and key_pem are paths to PEM-encoded certificate and key files.
// ALPN: offers "h2" and "http/1.1". Falls back to HTTP/1.1 if negotiated.
// Clients that negotiate "h2" get HTTP/2; others are served as HTTP/1.1.
server serve_tls(
    uint16_t port,
    const char* cert_pem,
    const char* key_pem,
    serve_options opts = {});

server serve_tls(
    const std::string& addr,
    uint16_t port,
    const char* cert_pem,
    const char* key_pem,
    serve_options opts = {});

#endif // CSP_TLS

} // namespace csp::http2
