// Non-channel surface microbenches for 🎯T38 (paper 35).
// Chrono-based (not nanobench): I/O and HTTP have high variance and
// long tails that make epoch-based nanobench unusably slow.

#include <csp/csp.h>
#include <csp/http.h>
#include <csp/net.h>
#include <csp/io.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace csp;
using clock_type = std::chrono::steady_clock;

static double ns_per(clock_type::duration d, int n) {
    return std::chrono::duration<double, std::nano>(d).count() / n;
}

// --- spawn empty: stack_pool acquire + fcontext make + schedule + exit ---
static void bench_spawn_empty(int n) {
    auto t0 = clock_type::now();
    for (int i = 0; i < n; ++i) {
        spawn([] {});
    }
    schedule();
    auto t1 = clock_type::now();
    std::printf("spawn/empty            %8.1f ns/op  (n=%d)\n",
                ns_per(t1 - t0, n), n);
}

// --- spawn that does one yield (context switch pair + run-queue) ---
static void bench_spawn_yield(int n) {
    auto t0 = clock_type::now();
    for (int i = 0; i < n; ++i) {
        spawn([] { yield(); });
    }
    schedule();
    auto t1 = clock_type::now();
    std::printf("spawn/yield            %8.1f ns/op  (n=%d)\n",
                ns_per(t1 - t0, n), n);
}

// --- TCP localhost echo: listen + dial + 1 write/read + close ---
static void bench_net_echo(int n) {
    auto t0 = clock_type::now();
    for (int i = 0; i < n; ++i) {
        chan<uint16_t> port_ch;
        spawn([w = std::move(port_ch.w)] {
            auto srv = net::listen(0);
            w << srv.port;
            net::connection conn;
            if (srv.connections >> conn) {
                auto rr = io::call_source(conn.source, 64);
                std::vector<uint8_t> buf;
                if (rr >> buf) {
                    conn.output << buf;
                }
            }
        });
        spawn([r = std::move(port_ch.r)] {
            uint16_t port;
            r >> port;
            auto conn = net::dial("127.0.0.1", port);
            std::vector<uint8_t> msg{'p', 'i', 'n', 'g'};
            conn.output << msg;
            auto rr = io::call_source(conn.source, 64);
            std::vector<uint8_t> got;
            rr >> got;
            conn.output = {};
            conn.source = {};
        });
        schedule();
    }
    auto t1 = clock_type::now();
    std::printf("net/echo-rtt           %8.1f ns/op  (n=%d, full setup)\n",
                ns_per(t1 - t0, n), n);
}

// --- Reuse one listener; many dial+echo on the same port ---
static void bench_net_echo_steady(int n) {
    chan<uint16_t> port_ch;
    chan<int> done_ch;
    spawn([w = std::move(port_ch.w), dw = done_ch.w.copy(), n] {
        auto srv = net::listen(0);
        w << srv.port;
        for (int i = 0; i < n; ++i) {
            net::connection conn;
            if (!(srv.connections >> conn)) break;
            auto rr = io::call_source(conn.source, 64);
            std::vector<uint8_t> buf;
            if (rr >> buf) {
                conn.output << buf;
            }
        }
        dw << 1;
    });
    spawn([r = std::move(port_ch.r), dr = std::move(done_ch.r), n] {
        uint16_t port;
        r >> port;
        auto t0 = clock_type::now();
        for (int i = 0; i < n; ++i) {
            auto conn = net::dial("127.0.0.1", port);
            std::vector<uint8_t> msg{'p', 'i', 'n', 'g'};
            conn.output << msg;
            auto rr = io::call_source(conn.source, 64);
            std::vector<uint8_t> got;
            rr >> got;
            conn.output = {};
            conn.source = {};
        }
        auto t1 = clock_type::now();
        std::printf("net/echo-steady        %8.1f ns/op  (n=%d, dial+rw)\n",
                    ns_per(t1 - t0, n), n);
        int d;
        dr >> d;
    });
    schedule();
}

// --- HTTP/1.1 GET over in-process serve + raw dial ---
static void bench_http_get(int n) {
    auto t0 = clock_type::now();
    for (int i = 0; i < n; ++i) {
        chan<uint16_t> port_ch;
        spawn([w = std::move(port_ch.w)] {
            auto srv = http::serve(0);
            w << srv.port;
            http::endpoint ep;
            if (srv.endpoints >> ep) {
                http::request req;
                if (ep.requests >> req) {
                    std::string body_str = "ok";
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
                "GET / HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Connection: close\r\n"
                "\r\n";
            bytes req_bytes(req.begin(), req.end());
            conn.output << req_bytes;
            // Drain response
            for (;;) {
                auto rr = io::call_source(conn.source, 4096);
                std::vector<uint8_t> chunk;
                if (!(rr >> chunk) || chunk.empty()) break;
            }
            conn.output = {};
            conn.source = {};
        });
        schedule();
    }
    auto t1 = clock_type::now();
    std::printf("http/get-setup         %8.1f ns/op  (n=%d, serve+GET)\n",
                ns_per(t1 - t0, n), n);
}

int main() {
    // Match typical user default; surfaces are not the channel flatness study.
    if (!std::getenv("CSP_MAXPROCS")) {
        setenv("CSP_MAXPROCS", "2", 0);
    }
    std::printf("# 🎯T38 surface microbenches (CSP_MAXPROCS=%s)\n",
                std::getenv("CSP_MAXPROCS") ? std::getenv("CSP_MAXPROCS") : "?");
    std::printf("# platform: macOS/Darwin focused driver; see paper 35\n");

    bench_spawn_empty(20'000);
    bench_spawn_yield(10'000);
    bench_net_echo(200);
    bench_net_echo_steady(500);
    bench_http_get(100);

    // Channel reference so readers can compare without re-opening paper 33
    // analysis — one epoch only, not a claim about scheduler thrash.
    {
        constexpr int BATCH = 50'000;
        chan<int> ch;
        auto t0 = clock_type::now();
        spawn([w = ch.w.copy()] {
            for (int i = 0; i < BATCH; ++i) w << i;
        });
        spawn([r = ch.r.copy()] {
            int n;
            for (int i = 0; i < BATCH; ++i) r >> n;
        });
        ch.release();
        schedule();
        auto t1 = clock_type::now();
        std::printf("channel/send-recv      %8.1f ns/op  (n=%d, reference)\n",
                    ns_per(t1 - t0, BATCH), BATCH);
    }

    csp::shutdown_runtime();
    return 0;
}
