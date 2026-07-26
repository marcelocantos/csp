#include "testutil.h"

#include <csp/net.h>

using namespace csp;

TEST_SUITE("net") {

TEST_CASE("listen-and-dial---basic-echo") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        // IPv4 any: dual-stack "::" + dial 127.0.0.1 is fragile on some
        // Windows dual-stack stacks (🎯T39). Bind IPv4 and dial loopback.
        auto srv = net::listen("0.0.0.0", 0);
        w << srv.port;

        net::connection conn;
        if (srv.connections >> conn) {
            for (;;) {
                auto rr = io::call_source(conn.source, 4096);
                std::vector<uint8_t> buf;
                if (!(rr >> buf)) break;
                if (!(conn.output << buf)) break;
            }
        }
    });

    spawn([r = std::move(port_ch.r)] {
        uint16_t port;
        r >> port;

        auto conn = net::dial("127.0.0.1", port);
        CHECK(!conn.remote_addr.empty());

        std::string msg = "hello csp\n";
        std::vector<uint8_t> data(msg.begin(), msg.end());
        conn.output << data;

        std::vector<uint8_t> response;
        auto rr = io::call_source(conn.source, 4096);
        rr >> response;
        std::string got(response.begin(), response.end());
        CHECK(got == msg);

        // Close connection — echo server sees EOF and exits.
        conn.output = {};
        conn.source = {};
    });

    schedule();
    csp::shutdown_runtime();
}

TEST_CASE("dial---connection-refused") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    spawn([] {
        CHECK_THROWS_AS(net::dial("127.0.0.1", 19999), csp::error);
    });

    schedule();
    csp::shutdown_runtime();
}

} // TEST_SUITE
