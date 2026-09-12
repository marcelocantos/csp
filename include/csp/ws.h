// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <csp/csp.h>
#include <csp/http.h>
#include <csp/net.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace csp::ws {

// --- Message types ---

enum class opcode : uint8_t {
    text   = 0x1,
    binary = 0x2,
    close  = 0x8,
    ping   = 0x9,
    pong   = 0xA,
};

struct message {
    opcode  op   = opcode::binary;  // text or binary
    bytes   data;                   // payload
};

// --- Endpoint pair returned by upgrade/connect ---

struct conn {
    reader<message> recv;   // inbound messages (text + binary only)
    writer<message> send;   // outbound messages

    // Abort without waiting for a peer Close echo. Idempotent; wakes blocked
    // socket and channel operations. The I/O imps join before fd release.
    // Asynchronous: endpoint death observes their subsequent completion.
    void close() const;

    struct impl;
    std::shared_ptr<impl> state;  // opaque lifetime control; do not access
};

struct options {
    // Maximum assembled inbound data message, including all fragments.
    // Zero preserves the unlimited default. Control frames stay <=125 bytes.
    // Exceeding this bound aborts the connection without a Close handshake.
    size_t max_message_size = 0;
};

// --- Server-side upgrade ---
//
// Call inside an HTTP request handler when you detect
// "Upgrade: websocket". Performs the HTTP 101 handshake and
// returns a conn for subsequent message exchange.
//
// Internally uses req.respond to send the 101 response, then reads
// req.hijack to take ownership of the raw socket from the HTTP layer.
//
// On error (bad handshake headers), sends 400 Bad Request via
// req.respond, drops req.hijack (so HTTP loop continues), and
// throws csp::error.
//
// Dropping send triggers a Close handshake (BLO: Close frame is
// sent to the peer, then recv closes after the peer's Close echo).

conn upgrade(http::request& req);
conn upgrade(http::request& req, options opts);

// --- Client-side connect ---
//
// Connects to ws://host[:port]/path and performs the opening
// handshake. Returns the conn pair. Throws csp::error on failure.
//
// url must use the "ws://" scheme (no wss:// in this release).

conn connect(const std::string& url);
conn connect(const std::string& url, options opts);

} // namespace csp::ws
