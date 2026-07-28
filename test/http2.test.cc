// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"

#include <csp/http2.h>
#include <csp/net.h>

#include <string>
#include <vector>

using namespace csp;

// --- Minimal HTTP/2 client for testing ---
//
// Sends the client connection preface followed by a HEADERS frame using
// nghttp2, reads the HEADERS + DATA response frames, and returns the
// response status and body.
//
// This test client uses nghttp2 on the client side too, which exercises
// the full HTTP/2 framing layer.

extern "C" {
#include <nghttp2/nghttp2.h>
}

namespace {

struct client_state {
    int32_t stream_id = 0;
    std::string status;
    std::vector<std::pair<std::string, std::string>> headers;
    csp::bytes body;
    bool done = false;
};

static int client_on_header(nghttp2_session* /*s*/,
                             const nghttp2_frame* frame,
                             const uint8_t* name, size_t namelen,
                             const uint8_t* value, size_t valuelen,
                             uint8_t /*flags*/,
                             void* user_data) {
    auto* cs = static_cast<client_state*>(user_data);
    if (frame->hd.stream_id != cs->stream_id) return 0;

    std::string k(reinterpret_cast<const char*>(name), namelen);
    std::string v(reinterpret_cast<const char*>(value), valuelen);

    if (k == ":status") cs->status = v;
    else cs->headers.emplace_back(std::move(k), std::move(v));
    return 0;
}

static int client_on_data_chunk(nghttp2_session* /*s*/,
                                uint8_t /*flags*/,
                                int32_t stream_id,
                                const uint8_t* data, size_t len,
                                void* user_data) {
    auto* cs = static_cast<client_state*>(user_data);
    if (stream_id != cs->stream_id) return 0;
    cs->body.insert(cs->body.end(), data, data + len);
    return 0;
}

static int client_on_stream_close(nghttp2_session* /*s*/,
                                  int32_t stream_id,
                                  uint32_t /*error_code*/,
                                  void* user_data) {
    auto* cs = static_cast<client_state*>(user_data);
    if (stream_id == cs->stream_id) cs->done = true;
    return 0;
}

// Simple blocking HTTP/2 client for tests.
// Sends one request, returns response status (int) and body.
struct h2_response {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Data source for request body.
struct req_body_data {
    const csp::bytes* body;
    size_t offset = 0;
};

static nghttp2_ssize req_body_read(nghttp2_session* /*s*/,
                                   int32_t /*stream_id*/,
                                   uint8_t* buf, size_t length,
                                   uint32_t* data_flags,
                                   nghttp2_data_source* source,
                                   void* /*user_data*/) {
    auto* bd = static_cast<req_body_data*>(source->ptr);
    size_t remaining = bd->body->size() - bd->offset;
    size_t n = std::min(length, remaining);
    if (n > 0) {
        std::memcpy(buf, bd->body->data() + bd->offset, n);
        bd->offset += n;
    }
    if (bd->offset >= bd->body->size()) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<nghttp2_ssize>(n);
}

h2_response h2_fetch(
    uint16_t port,
    const std::string& path,
    const std::string& method = "GET",
    const csp::bytes& req_body = {}) {

    auto conn = net::dial("127.0.0.1", port);

    client_state cs;

    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_header_callback(cbs, client_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cbs, client_on_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cbs, client_on_stream_close);

    nghttp2_session* session = nullptr;
    nghttp2_session_client_new(&session, cbs, &cs);
    nghttp2_session_callbacks_del(cbs);

    // Send client connection preface + SETTINGS.
    nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 10}};
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

    // Submit the request.
    std::string host = "127.0.0.1:" + std::to_string(port);
    std::string cl_str = std::to_string(req_body.size());
    std::vector<nghttp2_nv> nva = {
        {(uint8_t*)":method", (uint8_t*)method.c_str(), 7, method.size(),
         NGHTTP2_NV_FLAG_NO_COPY_NAME},
        {(uint8_t*)":path", (uint8_t*)path.c_str(), 5, path.size(),
         NGHTTP2_NV_FLAG_NO_COPY_NAME},
        {(uint8_t*)":scheme", (uint8_t*)"http", 7, 4,
         NGHTTP2_NV_FLAG_NO_COPY_NAME},
        {(uint8_t*)":authority", (uint8_t*)host.c_str(), 10, host.size(),
         NGHTTP2_NV_FLAG_NO_COPY_NAME},
    };
    if (!req_body.empty()) {
        nva.push_back({
            (uint8_t*)"content-length",
            (uint8_t*)cl_str.c_str(),
            14, cl_str.size(),
            NGHTTP2_NV_FLAG_NO_COPY_NAME,
        });
    }

    req_body_data bd{&req_body, 0};
    nghttp2_data_provider2* prd_ptr = nullptr;
    nghttp2_data_provider2 prd{};
    if (!req_body.empty()) {
        prd.source.ptr = &bd;
        prd.read_callback = req_body_read;
        prd_ptr = &prd;
    }

    int32_t sid = nghttp2_submit_request2(
        session, nullptr, nva.data(), nva.size(), prd_ptr, nullptr);
    cs.stream_id = sid;

    // I/O loop: flush pending frames, then read responses.
    auto flush_to_net = [&]() -> bool {
        const uint8_t* data = nullptr;
        for (;;) {
            ssize_t n = nghttp2_session_mem_send(session, &data);
            if (n == 0) break;
            if (n < 0) return false;
            csp::bytes chunk(data, data + n);
            if (!(conn.output << std::move(chunk))) return false;
        }
        return true;
    };

    flush_to_net();

    csp::bytes chunk;
    while (!cs.done) {
        auto rr = csp::io::call_source(conn.source, 4096);
        if (!(rr >> chunk)) break;
        nghttp2_session_mem_recv2(session, chunk.data(), chunk.size());
        flush_to_net();
    }

    // Send GOAWAY and close.
    nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
    flush_to_net();
    nghttp2_session_del(session);

    h2_response resp;
    if (!cs.status.empty()) resp.status = std::stoi(cs.status);
    resp.body.assign(cs.body.begin(), cs.body.end());
    resp.headers = std::move(cs.headers);
    return resp;
}

} // anonymous namespace

TEST_SUITE("http2") {

TEST_CASE("serve---basic-get") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve(0);
        w << srv.port;

        http2::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.streams >> req) {
                CHECK(req.method == http::method::GET);
                CHECK(req.url == "/hello");

                std::string body_str = "Hello, HTTP/2!";
                bytes body(body_str.begin(), body_str.end());
                req.respond << http::response{
                    200,
                    {{"content-type", "text/plain"}},
                    std::move(body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = h2_fetch(port, "/hello");
        CHECK(resp.status == 200);
        CHECK(resp.body == "Hello, HTTP/2!");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---post-with-body") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve(0);
        w << srv.port;

        http2::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.streams >> req) {
                CHECK(req.method == http::method::POST);
                CHECK(req.url == "/echo");

                std::string body_str(req.body.begin(), req.body.end());
                CHECK(body_str == "hello h2");

                req.respond << http::response{
                    200, {},
                    std::move(req.body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        std::string payload = "hello h2";
        bytes body(payload.begin(), payload.end());
        auto resp = h2_fetch(port, "/echo", "POST", body);
        CHECK(resp.status == 200);
        CHECK(resp.body == "hello h2");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---multiple-connections") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    // Server: handle two separate connections, one request each.
    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve(0);
        w << srv.port;

        int conn_count = 0;
        http2::endpoint ep;
        while (srv.endpoints >> ep) {
            ++conn_count;
            int n = conn_count;
            http::request req;
            if (ep.streams >> req) {
                std::string body = "response-" + std::to_string(n);
                req.respond << http::response{
                    200, {},
                    bytes(body.begin(), body.end())};
            }
            if (conn_count >= 2) break;
        }
        CHECK(conn_count == 2);
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        // Two sequential connections, one request each.
        auto r1 = h2_fetch(port, "/one");
        CHECK(r1.status == 200);
        CHECK(r1.body == "response-1");

        auto r2 = h2_fetch(port, "/two");
        CHECK(r2.status == 200);
        CHECK(r2.body == "response-2");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---404-response") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve(0);
        w << srv.port;

        http2::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.streams >> req) {
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

        auto resp = h2_fetch(port, "/missing");
        CHECK(resp.status == 404);
        CHECK(resp.body == "not found");
    });

    schedule();
    csp::shutdown_runtime();
}

// Regression: create_listener resolved with AF_INET6 + AI_PASSIVE, so an
// IPv4 literal bind address failed to resolve and serve() threw.
TEST_CASE("serve---ipv4-literal-bind-address") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve("127.0.0.1", 0);
        CHECK(srv.port != 0);
        w << srv.port;

        http2::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.streams >> req) {
                CHECK(req.url == "/v4");
                std::string body_str = "bound-on-127.0.0.1";
                bytes body(body_str.begin(), body_str.end());
                req.respond << http::response{200, {}, std::move(body)};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = h2_fetch(port, "/v4");
        CHECK(resp.status == 200);
        CHECK(resp.body == "bound-on-127.0.0.1");
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("serve---empty-body-response") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = http2::serve(0);
        w << srv.port;

        http2::endpoint ep;
        if (srv.endpoints >> ep) {
            http::request req;
            if (ep.streams >> req) {
                req.respond << http::response{204, {}, {}};
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto resp = h2_fetch(port, "/nodata");
        CHECK(resp.status == 204);
        CHECK(resp.body.empty());
    });

    schedule();
    csp::shutdown_runtime();
}

#if defined(CSP_TLS) && !defined(_WIN32)

// Regression: do_serve_tls's shutdown sentinel only self-connected under
// #ifndef _WIN32, so on Windows the accept loop had no way to observe the
// endpoints drop. The assertion here is structural: if the accept loop
// never wakes, its imp stays live and schedule() below never returns.
TEST_CASE("serve-tls---endpoints-drop-stops-accept-loop") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bool> done_ch;

    spawn([w = std::move(done_ch.w)] {
        auto srv = http2::serve_tls(
            0, "test/certs/server.crt", "test/certs/server.key");
        CHECK(srv.port != 0);

        // Drop the endpoints reader without ever accepting a connection.
        srv.endpoints = {};
        w << true;
    });

    spawn([r = std::move(done_ch.r)] {
        bool ok = false;
        CHECK((r >> ok));
        CHECK(ok);
    });

    schedule();
    csp::shutdown_runtime();
}

#endif // CSP_TLS && !_WIN32

} // TEST_SUITE
