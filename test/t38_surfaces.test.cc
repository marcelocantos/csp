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

// IPv4 any + dial 127.0.0.1 (🎯T39): dual-stack "::" is fragile on some
// Windows stacks; concrete 127.0.0.1 bind hung on Windows ARM VM.
net::listener listen_ipv4(uint16_t port = 0) {
    return net::listen("0.0.0.0", port);
}

}  // namespace

TEST_SUITE("T38Surfaces") {

TEST_CASE("paper-35-documents-required-surfaces") {
    // cwd must be the repo root (Makefile / win-validate WorkingDirectory).
    auto body = read_file("docs/papers/35-non-channel-performance-surfaces.md");

    CHECK(body.find("Reactor / I/O") != std::string::npos);
    CHECK(body.find("HTTP / TLS") != std::string::npos);
    CHECK(body.find("Stack pool / spawn") != std::string::npos);
    CHECK(body.find("Linux vs macOS") != std::string::npos);

    CHECK(body.find("**no action**") != std::string::npos);
    CHECK(body.find("**micro-opt opportunity**") != std::string::npos);

    CHECK(body.find("paper 33") != std::string::npos);
    bool closed_background =
        body.find("does not re-litigate") != std::string::npos
        || body.find("does **not** reopen") != std::string::npos;
    CHECK(closed_background);

    CHECK(body.find("No large, actionable opportunities were filed")
          != std::string::npos);

    bool lto_docs =
        body.find("not a bullseye target") != std::string::npos
        || body.find("docs only") != std::string::npos;
    CHECK(lto_docs);
}

TEST_CASE("shipped-surface-paths-still-run---spawn") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) spawn([] {});
    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("shipped-surface-paths-still-run---net-echo") {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // Same structure as net.test.cc listen-and-dial; IPv4 loopback (🎯T39).
    csp::shutdown_runtime();
    csp::set_maxprocs(2);
    chan<uint16_t> port_ch;
    spawn([w = std::move(port_ch.w)] {
        auto srv = listen_ipv4(0);
        w << srv.port;
        net::connection conn;
        if (srv.connections >> conn) {
            auto rr = io::call_source(conn.source, 64);
            std::vector<uint8_t> buf;
            if (rr >> buf) conn.output << buf;
        }
    });
    spawn([r = std::move(port_ch.r)] {
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
    csp::shutdown_runtime();
}

// HTTP GET smoke lives in http.test.cc (not built on Windows CMake yet).
// Net echo + spawn above exercise the reactor/stack surfaces from paper 35.

}  // TEST_SUITE
