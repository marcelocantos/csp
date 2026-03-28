#pragma once

#include <csp/csp.h>
#include <csp/io.h>
#include <csp/part/io.h>

#include <cstdint>
#include <string>
#include <utility>

namespace csp::net {

// --- Connection: RAII wrapper over a connected socket ---
//
// Provides split read/write channels via byte_reader/byte_writer.
// Closing (or dropping) the connection closes the underlying fd.

struct connection {
    io::socket_t fd;
    reader<std::vector<uint8_t>> input;   // bytes from peer
    writer<std::vector<uint8_t>> output;  // bytes to peer
    std::string remote_addr;              // peer address string

    connection() : fd(io::invalid_socket) {}
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

} // namespace csp::net
