#pragma once

#include <csp/csp.h>
#include <csp/io.h>
#include <csp/part/io.h>

#include <any>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace csp::net {

// --- Connection: RAII wrapper over a connected socket ---
//
// Provides split read/write channels via byte_reader/byte_writer.
// Closing (or dropping) the connection closes the underlying fd.

struct connection {
    io::fd_t fd;
    reader<std::vector<uint8_t>> input;   // bytes from peer
    writer<std::vector<uint8_t>> output;  // bytes to peer
    std::string remote_addr;              // peer address string

    connection() = default;
    connection(connection&&) = default;
    connection& operator=(connection&&) = default;
    connection(connection const&) = delete;
    connection& operator=(connection const&) = delete;
};

// --- Listener: TCP listener that produces connections ---

struct listen_options {
    int backlog = 128;
    bool reuse_addr = true;
    bool dual_stack = true;
};

struct listener {
    reader<connection> connections;  // read to accept
    uint16_t port;                  // actual bound port (useful with port 0)
    std::string local_addr;         // bound address string
};

// Create a TCP listener.  Use port 0 for OS-assigned ephemeral port.
// Dropping the connections reader stops accepting.
listener listen(uint16_t port, listen_options opts = {});
listener listen(const std::string& addr, uint16_t port,
                listen_options opts = {});

// --- dial: connect to a remote host ---
//
// Resolves the host, tries each address, returns the first successful
// connection.  The connection has a non-blocking fd with split I/O
// channels.
//
// Throws csp::error on failure (all addresses exhausted).
// Respects cancellation scope (throws canceled if cancelled).

connection dial(const std::string& host, uint16_t port);
connection dial(const std::string& host, const std::string& service);

// --- Unified server entry point (🎯T23.1) ------------------------------
//
// `csp::net::serve(port, {tls::enable(...), http::enable(), ...})` starts
// a server on `port` running the per-protocol stacks named in the option
// list. The option list is built from per-protocol `enable()` factories
// living in their own drop-in .cpp files — calling, e.g., `http::enable()`
// is what keeps `csp_http.cpp` (and llhttp) live in the link; an enable()
// that's not called leaves its protocol TU dead and the third-party
// library it depends on dropped by `-dead_strip` / `--gc-sections`.
//
// The opaque `protocol_option` struct carries an `apply()` function pointer
// — the only mechanism the front-door TU (`csp.cpp`) uses to invoke
// per-protocol behaviour — so Rule 5 of the per-protocol DCE model holds
// (docs/design/per-protocol-dist.md §5).
//
// Phase B (initial release): single-protocol case fully supported. ALPN
// negotiation for `tls` + `{http, http2}` and the interaction between
// multiple option types is follow-up work — see 🎯T23.1's context for
// the design space.

struct server;

// Context handed to each `protocol_option::apply()`. Apply functions stash
// their per-protocol server handles into `out->protocol_servers` (typed,
// retrievable via `std::any_cast`).
struct apply_context {
    uint16_t port;       // port the user asked for
    server*  out;        // destination for the protocol-specific server handles
};

// Opaque option produced by `csp::<proto>::enable()` factories. The
// front-door TU references only the `apply` function pointer — never the
// protocol's namespace by name — which keeps Rule 5 intact.
struct protocol_option {
    void* config = nullptr;
    void (*apply)(apply_context&, void* config) = nullptr;
    void (*destroy)(void* config) = nullptr;   // nullptr = no cleanup
};

// Unified server handle. Holds typed protocol-server objects (each
// protocol's `serve()` return type) inside a `std::any` so the front-
// door TU stays protocol-agnostic. Callers cast back with `std::any_cast`:
//
//     auto srv = csp::net::serve(8080, {csp::http::enable()});
//     auto& http_srv = std::any_cast<csp::http::server&>(srv.protocol_servers[0]);
//
struct server {
    uint16_t              port;
    std::vector<std::any> protocol_servers;
};

server serve(uint16_t port, std::initializer_list<protocol_option> opts);

} // namespace csp::net
