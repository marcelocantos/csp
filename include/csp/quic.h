#pragma once

#ifdef CSP_TLS

#include <csp/csp.h>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace csp::quic {

// --- stream_pair: bidirectional QUIC stream channels ---
//
// Each QUIC stream is exposed as a pair of CSP channels.
// The read end delivers byte chunks from the peer.
// Writing to the write end sends data to the peer.
// Dropping either end closes that direction of the stream.

struct stream_pair {
    reader<std::vector<uint8_t>> input;   // bytes from peer
    writer<std::vector<uint8_t>> output;  // bytes to peer
};

// --- connection: a QUIC connection bundle ---
//
// open_stream() negotiates a new bidirectional stream with the peer
// and returns a stream_pair.  The call blocks the calling imp until
// the stream is established.
//
// Dropping the connection object signals both sides that the connection
// is being torn down; in-flight streams are reset.

struct connection {
    // Open a new bidirectional stream.  Blocks until acknowledged.
    // Throws csp::error if the connection is closed.
    stream_pair open_stream();

    // Incoming streams initiated by the peer (for listen-side use).
    // Read from this to accept peer-initiated bidirectional streams.
    reader<stream_pair> incoming_streams;

    // Peer address string.
    std::string remote_addr;

    connection() = default;
    connection(connection&&) = default;
    connection& operator=(connection&&) = default;
    connection(connection const&) = delete;
    connection& operator=(connection const&) = delete;

    struct impl;
    std::shared_ptr<impl> impl_;
};

// --- listen_options ---

struct listen_options {
    // Maximum number of bidirectional streams a client may open
    // simultaneously (transport parameter).
    uint64_t max_streams_bidi = 128;

    // UDP receive buffer size in bytes (0 = OS default).
    int rcvbuf = 0;

    // PEM file paths for the server certificate and private key.
    // If empty, a self-signed certificate is generated at runtime
    // (for testing only — not suitable for production).
    std::string cert_pem;
    std::string key_pem;
};

// --- listener ---
//
// quic::listen(port) binds a UDP socket and starts accepting QUIC
// connections.  It returns a reader of connection bundles; each
// received value is a fully handshaked connection.
//
// Dropping the listener (or its connections reader) shuts down the
// listener imp.

struct listener {
    reader<connection> connections;  // read to accept
    uint16_t           port;         // actual bound port
    std::string        local_addr;

    listener() = default;
    listener(listener&&) = default;
    listener& operator=(listener&&) = default;
    listener(listener const&) = delete;
    listener& operator=(listener const&) = delete;

    ~listener() = default;

    // Internal: stop channel — writer held here, reader watched by the
    // listener imp. When listener is destroyed, stop_w_ dies → imp wakes.
    writer<std::monostate> stop_w_;
};

// Create a QUIC listener.  port=0 lets the OS assign an ephemeral port.
listener listen(uint16_t port, listen_options opts = {});

// --- dial_options ---

struct dial_options {
    // Maximum number of bidirectional streams this client may open.
    uint64_t max_streams_bidi = 128;

    // If true, attempt to use TLS session tickets for 0-RTT (deferred).
    bool enable_0rtt = false;

    // Custom certificate verification callback.  If null, no verification
    // is performed (acceptable for testing; set a real verifier in
    // production).
    // Signature: bool(const char* server_name,
    //                 const std::vector<std::vector<uint8_t>>& certs)
    using verify_fn = std::function<bool(
        const char*, const std::vector<std::vector<uint8_t>>&)>;
    verify_fn verify;
};

// Connect to a QUIC server.  Blocks until the TLS handshake completes.
// Throws csp::error on failure.
connection dial(const std::string& host, uint16_t port,
                dial_options opts = {});

} // namespace csp::quic

#endif // CSP_TLS
