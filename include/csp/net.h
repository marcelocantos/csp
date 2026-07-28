#pragma once

#include <csp/csp.h>
#include <csp/io.h>
#include <csp/part/io.h>
#include <csp/source.h>

#include <any>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace csp::net {

// --- Connection: RAII wrapper over a connected socket ---
//
// Reads go through `source` (🎯T17, pull-based): an io::source that lets
// the consumer drive read sizes.  Pull one chunk at a time with
// `source(n)`, or race a read against other channels with
// `io::call_source(source, n)`.  Writes go through `output` (push-based
// writer<bytes>).  Closing (or dropping) the connection closes the
// underlying fd(s).

struct connection {
    io::fd_t fd;
    io::source source;                    // bytes from peer (pull-based)
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

// --- Shared listener plumbing (🎯T48) ----------------------------------
//
// bind_listener / wake_listener / accept_loop single-source the TCP
// listener setup and accept-loop machinery that net::listen and the
// http/http2 servers previously each carried their own copy of.  The
// protocol servers layer their per-connection behaviour on top via the
// accept_loop callback; net::listen keeps its separate cancel-scope
// shutdown design and uses only bind_listener.

// Result of resolving + binding + listening on a TCP socket.
struct listen_result {
    io::fd_t listen_fd;
    uint16_t port = 0;          // actual bound port (useful with port 0)
    std::string local_addr;     // bound address string
    // Address to self-connect to when waking a blocked accept (see
    // wake_listener).  Derived from getsockname, so it tracks whatever
    // family and address the listener actually bound; wildcard binds are
    // substituted with the same-family loopback.
    sockaddr_storage wake_addr{};
    socklen_t wake_len = 0;
};

// Resolve `addr`, create a TCP socket, apply `opts`, bind, listen, and
// set non-blocking.  `resolve_err_prefix` prefixes the csp::error thrown
// on resolve failure (per-caller message, e.g. "http listen resolve
// failed: ").  Throws csp::error on any failure.
[[nodiscard]] listen_result bind_listener(const std::string& addr,
                                          uint16_t port,
                                          const listen_options& opts,
                                          const std::string& resolve_err_prefix);

// Unblock an accept() by opening a throwaway connection to the listener.
// The address must match the listener's own family: a listener bound to
// 127.0.0.1 is unreachable over ::1, and vice versa.
void wake_listener(const sockaddr_storage& addr, socklen_t len);

// Shared accept loop backing the protocol servers (http::serve,
// http2::serve, http2::serve_tls).  Spawns a producer imp named `descr`
// that accepts connections on lr.listen_fd and calls
// `on_connection(client_fd, remote_addr, out_copy)` for each accepted
// socket — the callback owns the fd and typically spawns a per-connection
// handler imp that eventually writes an Endpoint to out_copy.
//
// Shutdown: a sentinel imp watches for endpoint-reader death
// (prialt(~out_copy)), sets the stop flag, and self-connects via
// wake_listener to unblock a parked accept.  The loop exits on
// `!client_fd || stop` (closing any just-accepted fd) and closes the
// listener socket on exit.
template <typename Endpoint, typename F>
[[nodiscard]] reader<Endpoint> accept_loop(const listen_result& lr,
                                           const char* descr,
                                           F on_connection) {
    return spawn_producer<Endpoint>(
        [listen_fd = lr.listen_fd, wake_addr = lr.wake_addr,
         wake_len = lr.wake_len, descr,
         on_connection = std::move(on_connection)](
            writer<Endpoint> out) mutable {
            internal::descr("%s", descr);

            auto stop = std::make_shared<std::atomic<bool>>(false);

            // Sentinel: self-connect to unblock accept when reader dies.
            auto out_copy = out.copy();
            auto stop_flag = stop;
            csp::spawn([out_copy = std::move(out_copy),
                        stop_flag, wake_addr, wake_len, descr] {
                internal::descr("%s/sentinel", descr);
                prialt(~out_copy);
                stop_flag->store(true, std::memory_order_release);
                wake_listener(wake_addr, wake_len);
            });

            for (;;) {
                struct sockaddr_storage client_addr {};
                socklen_t client_len = sizeof(client_addr);
                io::fd_t client_fd = io::accept(
                    listen_fd,
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    &client_len);

                if (!client_fd ||
                    stop->load(std::memory_order_acquire)) {
                    if (client_fd) io::close(client_fd);
                    break;
                }

                auto remote = io::format_addr(
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    client_len);

                on_connection(client_fd, std::move(remote), out.copy());
            }
            io::close(listen_fd);
        });
}

// --- URL authority parsing (🎯T48) -------------------------------------
//
// Parses "<scheme_prefix>host[:port][/path]" — the shape shared by
// http::parse_url and ws::connect.  Handles "[IPv6]:port" bracketing.
// The scheme match is case-insensitive.  Throws csp::error on a scheme
// mismatch, malformed IPv6 brackets, or an empty host.

struct authority {
    std::string host;
    uint16_t port = 0;
    std::string path = "/";
};

[[nodiscard]] authority parse_authority(std::string_view url,
                                        std::string_view scheme_prefix,
                                        uint16_t default_port);

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
