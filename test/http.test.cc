// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"

#include <csp/http.h>
#include <csp/net.h>

#include <string>

using namespace csp;

TEST_SUITE("http") {

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

} // TEST_SUITE
