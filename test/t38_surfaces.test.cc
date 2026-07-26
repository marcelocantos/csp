// Structural + functional checks for 🎯T38 / paper 35.
// Proves the shipped measurement entry point still runs and the paper
// documents the required surfaces (no hard-coded ns/op floors).
//
// Uses only "csp.h" via testutil (works for both source and dist builds).

#include "testutil.h"

#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

using namespace csp;

namespace {

std::string read_file(const char* path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// Prefer IPv4 loopback for listen+dial pairs so dual-stack / IPv6-only
// hosts (and Windows without working IPv6 dual-stack) cannot leave the
// dialer blocked forever on the port channel — that hang was the
// Windows full-suite stall after paper-35 (🎯T39).
net::listener listen_loopback(uint16_t port = 0) {
    return net::listen("127.0.0.1", port);
}

}  // namespace

TEST_SUITE("T38Surfaces") {

TEST_CASE("paper-35-documents-required-surfaces") {
    // Drive the real artifact path relative to the repo root (tests run
    // with cwd = project root via Makefile / win-validate WorkingDirectory).
    auto body = read_file("docs/papers/35-non-channel-performance-surfaces.md");

    // Required surface headings (or equivalent).
    CHECK(body.find("Reactor / I/O") != std::string::npos);
    CHECK(body.find("HTTP / TLS") != std::string::npos);
    CHECK(body.find("Stack pool / spawn") != std::string::npos);
    CHECK(body.find("Linux vs macOS") != std::string::npos);

    // Each surface must carry an explicit verdict vocabulary from T38.
    CHECK(body.find("**no action**") != std::string::npos);
    CHECK(body.find("**micro-opt opportunity**") != std::string::npos);

    // Channel hot path closed as background, not re-litigated.
    CHECK(body.find("paper 33") != std::string::npos);
    bool closed_background =
        body.find("does not re-litigate") != std::string::npos
        || body.find("does **not** reopen") != std::string::npos;
    CHECK(closed_background);

    // No large follow-up targets claimed without filing.
    CHECK(body.find("No large, actionable opportunities were filed")
          != std::string::npos);

    // Dist LTO remains documentation-only.
    bool lto_docs =
        body.find("not a bullseye target") != std::string::npos
        || body.find("docs only") != std::string::npos;
    CHECK(lto_docs);
}

TEST_CASE("shipped-surface-paths-still-run") {
    // Functional smoke of the same entry points the t38_surfaces driver
    // uses — proves the measured code path is live, not a reimplementation.
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // RunStats installs global_exception_handler so imp failures become
    // CHECK failures instead of std::terminate / suite hang.
    RunStats stats;

    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    // Spawn empty (stack pool + schedule).
    {
        constexpr int N = 100;
        for (int i = 0; i < N; ++i) stats.spawn([] {});
        schedule();
    }

    // Net echo one-shot.
    {
        chan<uint16_t> port_ch;
        stats.spawn([w = std::move(port_ch.w)] {
            auto srv = listen_loopback(0);
            w << srv.port;
            net::connection conn;
            if (srv.connections >> conn) {
                auto rr = io::call_source(conn.source, 64);
                std::vector<uint8_t> buf;
                if (rr >> buf) conn.output << buf;
            }
        });
        stats.spawn([r = std::move(port_ch.r)] {
            uint16_t port;
            r >> port;
            auto conn = net::dial("127.0.0.1", port);
            std::vector<uint8_t> msg{'o', 'k'};
            conn.output << msg;
            auto rr = io::call_source(conn.source, 64);
            std::vector<uint8_t> got;
            bool ok = static_cast<bool>(rr >> got);
            REQUIRE(ok);
            REQUIRE(got.size() == 2);
            conn.output = {};
            conn.source = {};
        });
        schedule();
    }

    // HTTP GET one-shot (http.cc is linked on Windows; http.test.cc is
    // still CMake-excluded for broader suite reasons).
    {
        chan<uint16_t> port_ch;
        stats.spawn([w = std::move(port_ch.w)] {
            // Match net path: IPv4 loopback, not dual-stack "::".
            auto srv = http::serve("127.0.0.1", 0);
            w << srv.port;
            http::endpoint ep;
            if (srv.endpoints >> ep) {
                http::request req;
                if (ep.requests >> req) {
                    std::string body_str = "ok";
                    bytes body(body_str.begin(), body_str.end());
                    req.respond << http::response{
                        200, {{"Content-Type", "text/plain"}}, std::move(body)};
                }
            }
        });
        stats.spawn([r = std::move(port_ch.r)] {
            uint16_t port;
            r >> port;
            auto conn = net::dial("127.0.0.1", port);
            std::string req =
                "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            bytes req_bytes(req.begin(), req.end());
            conn.output << req_bytes;
            std::string response;
            for (;;) {
                auto rr = io::call_source(conn.source, 4096);
                std::vector<uint8_t> chunk;
                if (!(rr >> chunk) || chunk.empty()) break;
                response.append(chunk.begin(), chunk.end());
            }
            REQUIRE(response.find("200") != std::string::npos);
            conn.output = {};
            conn.source = {};
        });
        schedule();
    }

    csp::shutdown_runtime();
}

}  // TEST_SUITE
