// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"
#include "csp_headers.h"

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

// A real TCP peer with no WebSocket worker reading ahead or echoing Close.
// It remains connected until each test observes both server endpoints die.
struct raw_ws_peer {
    io::fd_t fd;
    explicit raw_ws_peer(uint16_t port) {
        fd = io::fd_t(::socket(AF_INET, SOCK_STREAM, 0));
        if (!fd) throw csp::error("test socket failed");
        io::set_nonblock(fd);
        int receive_buffer = 1024;
        ::setsockopt(fd.raw(), SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (io::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw csp::error("test connect failed");
        std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                              "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        if (io::write(fd, request.data(), request.size()) < 0) throw csp::error("test handshake write failed");
        std::string response;
        while (!response.ends_with("\r\n\r\n") && response.size() < 8192) {
            char byte;
            if (io::read(fd, &byte, 1) != 1) throw csp::error("test handshake read failed");
            response += byte;
        }
        CHECK(response.starts_with("HTTP/1.1 101"));
    }
    ~raw_ws_peer() { io::close(fd); }
    raw_ws_peer(raw_ws_peer const&) = delete;
    raw_ws_peer& operator=(raw_ws_peer const&) = delete;
};

TEST_SUITE("ws") {

TEST_CASE("ws---hard-close-releases-blocked-io-and-channel-waits") {
    for (int blocked = 0; blocked < 3; ++blocked) {
        CAPTURE(blocked);
        csp::shutdown_runtime();
        csp::set_maxprocs(2);
        spawn([blocked] {
            chan<> finished;
            chan<> sent;
            uint16_t port = serve_once([blocked, done_w = std::move(finished.w),
                                        sent_r = std::move(sent.r)](http::endpoint& ep) mutable {
                http::request req;
                REQUIRE(bool(ep.requests >> req));
                auto conn = ws::upgrade(req);
                if (blocked == 1) {
                    // A peer with a 1KiB receive window never reads this frame.
                    constexpr size_t blocked_payload = 16 * 1024 * 1024;
                    ws::message big{ws::opcode::binary, bytes(blocked_payload, 42)};
                    REQUIRE(bool(conn.send << std::move(big)));
                    ws::message next{ws::opcode::text, {'2'}};
                    CHECK(prialt(conn.send << next, after(std::chrono::milliseconds(50)) >> nullptr) == 1);
                } else if (blocked == 2) {
                    prialt(~sent_r);
                    csp::sleep(std::chrono::milliseconds(20));
                    // Do not receive the peer's message: the reader imp is parked
                    // forwarding it into the unbuffered user channel.
                } else {
                    csp::sleep(std::chrono::milliseconds(20));
                }
                conn.close();
                conn.close();  // idempotent
                CHECK(prialt(~conn.recv, after(std::chrono::seconds(2)) >> nullptr) == ~0);
                CHECK(prialt(~conn.send, after(std::chrono::seconds(2)) >> nullptr) == ~0);
                done_w = {};  // the peer still has not closed or echoed anything
            });
            spawn([port, blocked, done_r = std::move(finished.r), sent_w = std::move(sent.w)]() mutable {
                raw_ws_peer peer(port);
                if (blocked == 2) {
                    const uint8_t text[] = {0x81, 0x81, 0, 0, 0, 0, '1'};
                    CHECK(io::write(peer.fd, text, sizeof(text)) == sizeof(text));
                }
                sent_w = {};
                prialt(~done_r);
            });
        });
        await_completion();
        csp::shutdown_runtime();
    }
}

TEST_CASE("ws---receive-limit-covers-fragments-before-whole-payload-arrives") {
    for (int shape = 0; shape < 3; ++shape) {
        CAPTURE(shape);
        csp::shutdown_runtime();
        csp::set_maxprocs(2);
        spawn([shape] {
            chan<> finished;
            uint16_t port = serve_once([shape, done_w = std::move(finished.w)](http::endpoint& ep) mutable {
                http::request req;
                REQUIRE(bool(ep.requests >> req));
                auto conn = ws::upgrade(req, {.max_message_size = 4});
                ws::message msg;
                int received = prialt(conn.recv >> msg, after(std::chrono::seconds(2)) >> nullptr);
                if (shape == 0) {
                    CHECK(received == 0);
                    CHECK(msg.data == bytes{'1', '2', '3', '4'});
                    conn.close();
                } else {
                    CHECK(received == ~0);
                }
                CHECK(prialt(~conn.send, after(std::chrono::seconds(2)) >> nullptr) == ~0);
                done_w = {};
            });
            spawn([port, shape, done_r = std::move(finished.r)] {
                raw_ws_peer peer(port);
                if (shape == 0) {
                    // A TCP split inside the frame header must not look like EOF.
                    const uint8_t first[] = {0x81};
                    CHECK(io::write(peer.fd, first, sizeof(first)) == sizeof(first));
                    csp::sleep(std::chrono::milliseconds(20));
                    const uint8_t rest[] = {0x84, 0, 0, 0, 0, '1', '2', '3', '4'};
                    CHECK(io::write(peer.fd, rest, sizeof(rest)) == sizeof(rest));
                } else if (shape == 1) {
                    // Advertise five bytes but send only one. Waiting for the
                    // complete frame before enforcing the limit would hang.
                    const uint8_t frame[] = {0x81, 0x85, 0, 0, 0, 0, '1'};
                    CHECK(io::write(peer.fd, frame, sizeof(frame)) == sizeof(frame));
                } else {
                    const uint8_t frames[] = {0x01, 0x83, 0, 0, 0, 0, '1', '2', '3',
                                              0x80, 0x82, 0, 0, 0, 0, '4'};
                    CHECK(io::write(peer.fd, frames, sizeof(frames)) == sizeof(frames));
                }
                prialt(~done_r);
            });
        });
        await_completion();
        csp::shutdown_runtime();
    }
}

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

    await_completion();
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

    await_completion();
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

    await_completion();
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

    await_completion();
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
        for (;;) {
            auto rr = io::call_source(conn.source, 4096);
            bytes chunk;
            if (!(rr >> chunk)) break;
            response.append(chunk.begin(), chunk.end());
        }
        CHECK(response.find("400") != std::string::npos);
    });

    await_completion();
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

    await_completion();
    csp::shutdown_runtime();
}

} // TEST_SUITE
