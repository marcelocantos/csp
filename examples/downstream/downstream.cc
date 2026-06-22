// Downstream link-test sample for the pre-built CSP library artefacts (🎯T24).
//
// This file is the "external user" of CSP. It #includes only the published
// public header (csp.h) and links against the published static/shared
// libraries (libcsp, libcsp_http, libllhttp) — NO CSP sources and NO
// vendored third-party sources live in this directory. If it links and runs,
// the release artefacts and the documented link incantation are correct.
//
// It exercises two layers:
//   1. Core channels (libcsp): a tiny producer/consumer round-trip.
//   2. HTTP/1.1 (libcsp_http + libllhttp): an in-process server answering one
//      request from an in-process client, proving the protocol library and
//      its vendored dep are wired correctly end-to-end.

#include "csp.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    int exit_code = 0;

    csp::run([&] {
        // --- 1. Core channels: producer → consumer. ---
        auto [w, r] = csp::chan<int>{};
        csp::spawn([o = std::move(w)]() mutable { o << 42; });
        int got = r.read();
        if (got != 42) {
            std::fprintf(stderr, "channel round-trip failed: got %d\n", got);
            exit_code = 1;
            return;
        }
        std::puts("core channels: OK (received 42)");

        // --- 2. HTTP/1.1: in-process server + client round-trip. ---
        // Server imp: bind an OS-assigned port, accept exactly one
        // connection, answer its one request with a fixed body, then return.
        // The client sends Connection: close, so the server's request reader
        // closes after one request and every imp finishes — run() returns
        // cleanly with no imps left blocked.
        auto port_ch = csp::chan<uint16_t>{};
        csp::spawn([pw = std::move(port_ch.w)]() mutable {
            auto srv = csp::http::serve(0);
            pw << srv.port;
            pw = {};

            csp::http::endpoint ep;
            if (srv.endpoints >> ep) {
                csp::http::request req;
                if (ep.requests >> req) {
                    req.respond << csp::http::response{
                        200, {}, csp::bytes{'h', 'e', 'l', 'l', 'o'}};
                }
            }
        });

        uint16_t port = port_ch.r.read();
        port_ch.r = {};

        auto resp = csp::http::get(
            "http://127.0.0.1:" + std::to_string(port) + "/",
            {{"Connection", "close"}});
        std::string body(resp.body.begin(), resp.body.end());
        if (resp.status != 200 || body != "hello") {
            std::fprintf(stderr, "http round-trip failed: status=%d body=%s\n",
                         resp.status, body.c_str());
            exit_code = 1;
        } else {
            std::printf("http/1.1: OK (status %d, body \"%s\")\n",
                        resp.status, body.c_str());
        }
    });

    // Wind down the reactor / blocking-pool threads before main returns so
    // static destructors don't race live runtime threads at process exit.
    csp::shutdown_runtime();

    if (exit_code == 0) std::puts("downstream sample: OK");
    return exit_code;
}
