// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"

#include <csp/ws.h>
#include <csp/http.h>
#include <csp/net.h>

#include <string>
#include <vector>

using namespace csp;

// Helper: spin up an http::server and return the port.
// The provided handler lambda receives (http::endpoint&).
template <typename Handler>
static uint16_t serve_once(Handler h) {
    chan<uint16_t> port_ch;
    spawn([w = std::move(port_ch.w), h = std::move(h)]() mutable {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            h(ep);
        }
    });
    uint16_t port;
    port_ch.r >> port;
    return port;
}

TEST_SUITE("ws") {

// --- Basic text echo ---
TEST_CASE("ws---text-echo") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    // Server: upgrade and echo text messages.
    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        // Upgrade to WebSocket.
        auto conn = ws::upgrade(req);

        // Echo one message back.
        ws::message msg;
        if (conn.recv >> msg) {
            conn.send << std::move(msg);
        }
    });

    // Client: connect and exchange a text message.
    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = ws::connect("ws://127.0.0.1:" + std::to_string(port) + "/");

        std::string text = "Hello, WebSocket!";
        ws::message out;
        out.op   = ws::opcode::text;
        out.data = bytes(text.begin(), text.end());
        conn.send << std::move(out);

        ws::message in;
        if (conn.recv >> in) {
            CHECK(in.op == ws::opcode::text);
            std::string got(in.data.begin(), in.data.end());
            CHECK(got == text);
        } else {
            FAIL("recv channel closed unexpectedly");
        }
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Binary message ---
TEST_CASE("ws---binary-message") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        auto conn = ws::upgrade(req);

        ws::message msg;
        if (conn.recv >> msg) {
            CHECK(msg.op == ws::opcode::binary);
            conn.send << std::move(msg);
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = ws::connect("ws://127.0.0.1:" + std::to_string(port) + "/ws");

        ws::message out;
        out.op   = ws::opcode::binary;
        out.data = {0x01, 0x02, 0x03, 0xFF, 0x00};
        conn.send << out;

        ws::message in;
        if (conn.recv >> in) {
            CHECK(in.op == ws::opcode::binary);
            CHECK(in.data == out.data);
        } else {
            FAIL("recv channel closed unexpectedly");
        }
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Multiple messages ---
TEST_CASE("ws---multiple-messages") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;
    constexpr int N = 5;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        auto conn = ws::upgrade(req);

        // Echo all messages.
        ws::message msg;
        while (conn.recv >> msg) {
            conn.send << std::move(msg);
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = ws::connect("ws://127.0.0.1:" + std::to_string(port) + "/");

        for (int i = 0; i < N; ++i) {
            std::string text = "msg-" + std::to_string(i);
            ws::message out;
            out.op   = ws::opcode::text;
            out.data = bytes(text.begin(), text.end());
            conn.send << std::move(out);

            ws::message in;
            if (!(conn.recv >> in)) {
                FAIL("recv closed early");
                break;
            }
            std::string got(in.data.begin(), in.data.end());
            CHECK(got == text);
        }
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Close handshake via writer death (BLO) ---
TEST_CASE("ws---close-on-writer-death") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;
    chan<bool> done_ch;

    spawn([w = std::move(port_ch.w), d = std::move(done_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        auto conn = ws::upgrade(req);

        // Drain messages until recv closes (client dropped send).
        ws::message msg;
        while (conn.recv >> msg) {}

        // Signal completion.
        d << true;
    });

    spawn([r = std::move(port_ch.r), dr = std::move(done_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = ws::connect("ws://127.0.0.1:" + std::to_string(port) + "/");

        // Send one message then drop sender → triggers Close handshake.
        std::string text = "goodbye";
        ws::message out;
        out.op   = ws::opcode::text;
        out.data = bytes(text.begin(), text.end());
        conn.send << std::move(out);

        // Drop send — BLO: Close frame sent.
        conn.send = {};

        // Wait for server to see the close.
        bool done = false;
        dr >> done;
        CHECK(done);
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Bad upgrade request returns 400 ---
TEST_CASE("ws---bad-upgrade-no-key") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        try {
            auto conn = ws::upgrade(req);
            FAIL("should have thrown");
        } catch (const csp::error&) {
            // Expected: missing Sec-WebSocket-Key
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        // Send an upgrade request missing Sec-WebSocket-Key.
        std::string req =
            "GET /ws HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        // Read the response and check it's 400.
        std::string response;
        bytes chunk;
        while (conn.input >> chunk) {
            response.append(chunk.begin(), chunk.end());
        }
        CHECK(response.find("400") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Large message ---
TEST_CASE("ws---large-message") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;
    static constexpr size_t MSG_SIZE = 64 * 1024; // 64 KiB

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (!(srv.endpoints >> ep)) return;

        http::request req;
        if (!(ep.requests >> req)) return;

        auto conn = ws::upgrade(req);

        ws::message msg;
        if (conn.recv >> msg) {
            CHECK(msg.data.size() == MSG_SIZE);
            conn.send << std::move(msg);
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = ws::connect("ws://127.0.0.1:" + std::to_string(port) + "/");

        ws::message out;
        out.op   = ws::opcode::binary;
        out.data.resize(MSG_SIZE);
        for (size_t i = 0; i < MSG_SIZE; ++i) {
            out.data[i] = static_cast<uint8_t>(i & 0xFF);
        }
        conn.send << out;

        ws::message in;
        if (conn.recv >> in) {
            CHECK(in.data.size() == MSG_SIZE);
            CHECK(in.data == out.data);
        } else {
            FAIL("recv closed unexpectedly");
        }
    });

    schedule();
    csp::shutdown_runtime();
}

} // TEST_SUITE
