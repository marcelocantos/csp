// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <csp/csp.h>
#include <csp/net.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace csp::http {

// --- HTTP method enum ---

enum class method {
    GET, HEAD, POST, PUT, DELETE_, PATCH, OPTIONS, CONNECT, TRACE,
};

const char* method_name(method m);

// --- Response: what the handler sends back ---

struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
};

// --- Request: a parsed HTTP/1.1 request ---
//
// Delivered to the handler as soon as the request headers are complete.
// Body chunks arrive via `body_stream` (a reader<bytes> channel that
// closes at end-of-body).  For no-body requests the channel is already
// closed when the request is delivered.
//
// Each request carries a per-request response channel: write exactly
// one response to `respond`, then let it drop.

struct request {
    http::method method = method::GET;
    std::string url;
    uint8_t version_major = 1;
    uint8_t version_minor = 1;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;                 // accumulated body; empty until drain() is called
    bool keep_alive = true;

    // Streaming body: push-based chunks as they arrive from the network.
    // Closes when the body is complete (EOF).  For no-body requests the
    // channel is already closed when the request is delivered.
    reader<bytes> body_stream;

    // Write exactly one response for this request.
    writer<response> respond;

    // WebSocket / protocol-upgrade hijack.
    //
    // After writing a 101 Switching Protocols response via `respond`,
    // read the raw fd from this channel to take ownership of the
    // connection socket. The HTTP connection loop exits as soon as
    // the fd is claimed. Any bytes read by the HTTP parser beyond the
    // upgrade request are delivered in `leftover` so the caller can
    // replay them.
    //
    // Normal (non-upgrade) handlers must not touch this channel.
    struct hijack_result {
        io::fd_t  fd;       // connection socket (non-blocking)
        bytes     leftover; // bytes consumed by HTTP parser but not yet used
    };
    reader<hijack_result> hijack;

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

// --- Per-connection endpoint ---

struct endpoint {
    reader<request> requests;       // one per HTTP request on this connection
    std::string remote_addr;
};

// --- Server options ---

struct serve_options {
    net::listen_options listen = {};
    size_t max_header_size = 8192;
    size_t read_chunk_size = 4096;
};

// --- Server entry point ---

struct server {
    reader<endpoint> endpoints;     // one per accepted connection
    uint16_t port;                  // actual bound port
    std::string local_addr;
};

// Start an HTTP/1.1 server on the given port.
// Returns a server whose endpoints reader yields one endpoint per
// connection. Dropping the reader stops accepting.
server serve(uint16_t port, serve_options opts = {});
server serve(const std::string& addr, uint16_t port, serve_options opts = {});

// --- Client ---

struct fetch_options {
    size_t read_chunk_size = 4096;
};

// Perform an HTTP/1.1 request.  Blocks the calling imp until the
// full response (headers + body) has been received.
//
// url is "http://host[:port]/path" — only the http scheme is
// supported (use csp::tls for HTTPS).  Default port is 80.
//
// Throws csp::error on connection failure, DNS failure, or parse error.
response fetch(
    method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bytes body = {},
    fetch_options opts = {});

// Convenience wrappers.
inline response get(
    const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    fetch_options opts = {}) {
    return fetch(method::GET, url, std::move(headers), {}, opts);
}

inline response post(
    const std::string& url, bytes body,
    std::vector<std::pair<std::string, std::string>> headers = {},
    fetch_options opts = {}) {
    return fetch(method::POST, url, std::move(headers), std::move(body), opts);
}

} // namespace csp::http
