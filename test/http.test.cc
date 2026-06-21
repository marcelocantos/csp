// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"

#include <csp/http.h>
#include <csp/net.h>

#include <cstring>
#include <string>

using namespace csp;

namespace {

// Read one complete HTTP/1.1 response (status line + headers + a
// Content-Length-delimited body) from a connection's pull-based source,
// carrying any bytes past the response boundary forward in `buf` for the
// next call (so it works across keep-alive responses).
std::string read_one_response(io::source& src, std::string& buf) {
    for (;;) {
        auto hdr_end = buf.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            size_t content_len = 0;
            auto cl = buf.find("Content-Length:");
            if (cl != std::string::npos && cl < hdr_end) {
                auto le = buf.find("\r\n", cl);
                content_len = static_cast<size_t>(
                    std::stoul(buf.substr(cl + 15, le - (cl + 15))));
            }
            size_t total = hdr_end + 4 + content_len;
            if (buf.size() >= total) {
                std::string resp = buf.substr(0, total);
                buf.erase(0, total);
                return resp;
            }
        }
        auto rr = io::call_source(src, 4096);
        bytes chunk;
        if (!(rr >> chunk)) {  // EOF: return whatever we have.
            std::string resp = std::move(buf);
            buf.clear();
            return resp;
        }
        buf.append(chunk.begin(), chunk.end());
    }
}

} // namespace

TEST_SUITE("http") {

// 🎯T23.1 — csp::net::serve(port, {csp::http::enable()}) factory API.
TEST_CASE("enable---net-serve-with-http-enable") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        // Unified net::serve walks the option list and starts each
        // protocol's server. http::enable() apply() calls csp::http::serve
        // internally and stashes its csp::http::server (wrapped in a
        // shared_ptr because std::any requires copyable).
        auto srv = net::serve(0, {http::enable()});
        REQUIRE(srv.protocol_servers.size() == 1);
        auto http_srv =
            std::any_cast<std::shared_ptr<http::server>>(srv.protocol_servers[0]);
        REQUIRE(http_srv != nullptr);
        w << http_srv->port;

        http::endpoint ep;
        if (http_srv->endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.url == "/factory");
                std::string body_str = "factory-OK";
                bytes body(body_str.begin(), body_str.end());
                req.respond << http::response{
                    200,
                    {{"Content-Type", "text/plain"}},
                    std::move(body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);
        std::string req =
            "GET /factory HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        std::string response;
        bytes chunk;
        while (conn.input >> chunk) {
            response.append(chunk.begin(), chunk.end());
        }

        CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(response.find("factory-OK") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---basic-get") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::GET);
                CHECK(req.url == "/hello");
                CHECK(!req.keep_alive);

                std::string body_str = "Hello, CSP!";
                bytes body(body_str.begin(), body_str.end());
                req.respond << http::response{
                    200,
                    {{"Content-Type", "text/plain"}},
                    std::move(body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        std::string req =
            "GET /hello HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        std::string response;
        bytes chunk;
        while (conn.input >> chunk) {
            response.append(chunk.begin(), chunk.end());
        }

        CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(response.find("Content-Type: text/plain") != std::string::npos);
        CHECK(response.find("Hello, CSP!") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---post-with-body") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/echo");

                req.drain();
                std::string body_str(req.body.begin(), req.body.end());
                CHECK(body_str == "request body data");

                req.respond << http::response{
                    200,
                    {{"Content-Type", "application/octet-stream"}},
                    std::move(req.body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        std::string body_str = "request body data";
        std::string req =
            "POST /echo HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: " + std::to_string(body_str.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body_str;
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        std::string response;
        bytes chunk;
        while (conn.input >> chunk) {
            response.append(chunk.begin(), chunk.end());
        }

        CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(response.find("request body data") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---keep-alive") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            int count = 0;
            http::request req;
            while (ep.requests >> req) {
                ++count;
                std::string body = "response " + std::to_string(count);
                req.respond << http::response{
                    200, {},
                    bytes(body.begin(), body.end())};

                if (count >= 2) break;
            }
            CHECK(count == 2);
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        // Send both requests, then read both responses.
        // The server handles them sequentially (request-response-
        // request-response) so we send one at a time.
        std::string leftover;
        for (int i = 0; i < 2; ++i) {
            std::string req =
                "GET /test HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "\r\n";
            bytes req_bytes(req.begin(), req.end());
            conn.output << req_bytes;

            std::string response = leftover;
            leftover.clear();
            bytes chunk;
            for (;;) {
                // Check if we already have a complete response.
                auto body_start = response.find("\r\n\r\n");
                if (body_start != std::string::npos) {
                    auto cl_pos = response.find("Content-Length: ");
                    if (cl_pos != std::string::npos) {
                        auto cl_end = response.find("\r\n", cl_pos);
                        auto cl_str = response.substr(
                            cl_pos + 16, cl_end - cl_pos - 16);
                        auto content_len =
                            static_cast<size_t>(std::stoul(cl_str));
                        auto header_end = body_start + 4;
                        auto total = header_end + content_len;
                        if (response.size() >= total) {
                            leftover = response.substr(total);
                            response.resize(total);
                            break;
                        }
                    }
                }
                if (!(conn.input >> chunk)) break;
                response.append(chunk.begin(), chunk.end());
            }
            CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
            std::string expected = "response " + std::to_string(i + 1);
            CHECK(response.find(expected) != std::string::npos);
        }
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---request-header-lookup") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.header("host") == "localhost:1234");
                CHECK(req.header("X-Custom") == "test-value");
                CHECK(req.header("nonexistent").empty());

                req.respond << http::response{200, {}, {}};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        std::string req =
            "GET / HTTP/1.1\r\n"
            "Host: localhost:1234\r\n"
            "X-Custom: test-value\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        bytes chunk;
        while (conn.input >> chunk) {}
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---method-name") {
    CHECK(std::string(http::method_name(http::method::GET)) == "GET");
    CHECK(std::string(http::method_name(http::method::POST)) == "POST");
    CHECK(std::string(http::method_name(http::method::DELETE_)) == "DELETE");
}

TEST_CASE("serve---streaming-body") {
    // Verifies that body_stream delivers the request body as a readable
    // stream without the handler calling drain().
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/stream");

                // Read body_stream directly without calling drain().
                std::string accumulated;
                bytes chunk;
                while (req.body_stream >> chunk) {
                    accumulated.append(chunk.begin(), chunk.end());
                }
                CHECK(accumulated == "streamed body data");

                bytes resp_body(accumulated.begin(), accumulated.end());
                req.respond << http::response{
                    200,
                    {{"Content-Type", "application/octet-stream"}},
                    std::move(resp_body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        std::string body_str = "streamed body data";
        std::string req =
            "POST /stream HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: " + std::to_string(body_str.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body_str;
        bytes req_bytes(req.begin(), req.end());
        conn.output << req_bytes;

        std::string response;
        bytes chunk;
        while (conn.input >> chunk) {
            response.append(chunk.begin(), chunk.end());
        }

        CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(response.find("streamed body data") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---streaming-body-large") {
    // 🎯T17.5: a body larger than the read chunk is delivered to the handler
    // as multiple body_stream chunks (genuinely streamed, not pre-buffered),
    // each bounded by the read chunk size — so memory stays bounded by the
    // chunk size rather than the body size.
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    constexpr size_t body_size = 100 * 1024;  // 100 KiB >> default 4096 read chunk

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w), body_size] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/upload");

                size_t total = 0;
                int chunk_count = 0;
                size_t max_chunk = 0;
                bytes chunk;
                while (req.body_stream >> chunk) {
                    total += chunk.size();
                    if (chunk.size() > max_chunk) max_chunk = chunk.size();
                    ++chunk_count;
                }
                CHECK(total == body_size);
                CHECK(chunk_count > 1);     // proves it streamed, not buffered
                CHECK(max_chunk <= 4096);   // bounded by the read chunk size

                std::string ok = "ok";
                req.respond << http::response{
                    200, {}, bytes(ok.begin(), ok.end())};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);

        std::string head =
            "POST /upload HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: " + std::to_string(body_size) + "\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req_bytes(head.begin(), head.end());
        req_bytes.insert(req_bytes.end(), body_size, 'x');
        conn.output << std::move(req_bytes);

        std::string buf;
        auto response = read_one_response(conn.source, buf);
        CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---early-response-without-draining") {
    // 🎯T17.5: a handler that responds without reading the body (e.g. an
    // early 413 rejection) must not deadlock the connection, and the server
    // must drain the unread body off the wire so the next keep-alive request
    // stays in sync.
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    constexpr size_t body_size = 16 * 1024;  // spans several read chunks

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            int count = 0;
            http::request req;
            while (ep.requests >> req) {
                ++count;
                if (count == 1) {
                    // Reject WITHOUT touching req.body_stream.
                    CHECK(req.url == "/big");
                    std::string msg = "too big";
                    req.respond << http::response{
                        413, {}, bytes(msg.begin(), msg.end())};
                } else {
                    // The second request proves the connection survived the
                    // un-drained first body and stayed byte-aligned.
                    CHECK(req.url == "/ok");
                    std::string msg = "fine";
                    req.respond << http::response{
                        200, {}, bytes(msg.begin(), msg.end())};
                    break;
                }
            }
            CHECK(count == 2);
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);
        std::string buf;

        // Request 1: large body, keep-alive; the handler won't read it.
        std::string head1 =
            "POST /big HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: " + std::to_string(body_size) + "\r\n"
            "\r\n";
        bytes req1(head1.begin(), head1.end());
        req1.insert(req1.end(), body_size, 'x');
        conn.output << std::move(req1);

        auto resp1 = read_one_response(conn.source, buf);
        CHECK(resp1.find("413") != std::string::npos);

        // Request 2: small GET; expect 200 — connection is still in sync.
        std::string req2 =
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        bytes req2_bytes(req2.begin(), req2.end());
        conn.output << std::move(req2_bytes);

        auto resp2 = read_one_response(conn.source, buf);
        CHECK(resp2.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(resp2.find("fine") != std::string::npos);
    });

    schedule();
    csp::shutdown_runtime();
}

// --- Client tests ---

TEST_CASE("fetch---basic-get") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    // Server: respond to one GET.
    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::GET);
                CHECK(req.url == "/api/data");

                std::string body_str = R"({"ok":true})";
                bytes body(body_str.begin(), body_str.end());
                req.respond << http::response{
                    200,
                    {{"Content-Type", "application/json"}},
                    std::move(body)};
            }
        }
    });

    // Client: fetch from the server.
    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = http::get("http://127.0.0.1:" + std::to_string(port) +
                              "/api/data");

        CHECK(resp.status == 200);
        std::string body(resp.body.begin(), resp.body.end());
        CHECK(body == R"({"ok":true})");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("fetch---post-with-body") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/echo");

                req.drain();
                std::string body_str(req.body.begin(), req.body.end());
                CHECK(body_str == "hello from client");

                req.respond << http::response{
                    201,
                    {{"X-Echo", "true"}},
                    std::move(req.body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        std::string payload = "hello from client";
        bytes body(payload.begin(), payload.end());
        auto resp = http::post(
            "http://127.0.0.1:" + std::to_string(port) + "/echo",
            std::move(body));

        CHECK(resp.status == 201);
        std::string resp_body(resp.body.begin(), resp.body.end());
        CHECK(resp_body == "hello from client");
    });

    schedule();
    csp::shutdown_runtime();
}

// 🎯T17 streaming-body overload: body comes from an io::source instead
// of being materialised as `bytes` up front.  Server side is unchanged.
TEST_CASE("fetch---streaming-body-source") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/stream");
                req.drain();
                std::string body_str(req.body.begin(), req.body.end());
                CHECK(body_str == "streamedpayloaddata!");
                req.respond << http::response{200, {}, std::move(req.body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        // Build a source from a chan<bytes> that ships three chunks
        // adding up to 20 bytes total.
        chan<io::read_request> ch;
        spawn([req_r = std::move(ch.r)]() mutable {
            internal::descr("test::streaming_body");
            const char* chunks[] = {"streamed", "payload", "data!"};
            size_t i = 0;
            io::read_request req;
            while (i < 3 && req_r >> req) {
                bytes out(chunks[i], chunks[i] + std::strlen(chunks[i]));
                req.reply << std::move(out);
                ++i;
            }
        });

        auto resp = http::post(
            "http://127.0.0.1:" + std::to_string(port) + "/stream",
            std::move(ch.w),
            /*body_length=*/ 20);

        CHECK(resp.status == 200);
        std::string resp_body(resp.body.begin(), resp.body.end());
        CHECK(resp_body == "streamedpayloaddata!");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("fetch---custom-headers") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                CHECK(req.header("Authorization") == "Bearer tok123");
                CHECK(req.header("Accept") == "text/plain");
                req.respond << http::response{200, {}, {}};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = http::get(
            "http://127.0.0.1:" + std::to_string(port) + "/",
            {{"Authorization", "Bearer tok123"},
             {"Accept", "text/plain"}});

        CHECK(resp.status == 200);
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("fetch---404-response") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http::serve(0);
        w << srv.port;

        http::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.requests >> req) {
                std::string body = "not found";
                req.respond << http::response{
                    404, {},
                    bytes(body.begin(), body.end())};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = http::get(
            "http://127.0.0.1:" + std::to_string(port) + "/missing");
        CHECK(resp.status == 404);
        std::string body(resp.body.begin(), resp.body.end());
        CHECK(body == "not found");
    });

    schedule();
    csp::shutdown_runtime();
}

} // TEST_SUITE
