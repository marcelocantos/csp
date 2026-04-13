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
// The request body is buffered in memory. For the request body to be
// streamed, a future API (with a reader<bytes> body) will be added.
// Each request carries a per-request response channel: write exactly
// one response to `respond`, then let it drop.

struct request {
    http::method method = method::GET;
    std::string url;
    uint8_t version_major = 1;
    uint8_t version_minor = 1;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;                 // complete request body
    bool keep_alive = true;

    // Write exactly one response for this request.
    writer<response> respond;

    // Convenience: find first header value by case-insensitive name.
    // Returns empty string if not found.
    std::string header(const std::string& name) const;

    // Content-Length, or -1 if absent.
    int64_t content_length() const;
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
