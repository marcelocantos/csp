#include "testutil.h"

#include <csp/net.h>

using namespace csp;

TEST_SUITE("net") {

TEST_CASE("listen and dial - basic echo") {
    RunStats stats;

    chan<uint16_t> port_ch;

    spawn([w = std::move(port_ch.w)] {
        auto srv = net::listen(0);
        w << srv.port;

        net::connection conn;
        if (srv.connections >> conn) {
            std::vector<uint8_t> buf;
            while (conn.input >> buf) {
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
        conn.input >> response;
        std::string got(response.begin(), response.end());
        CHECK(got == msg);

        // Close connection — echo server sees EOF and exits.
        conn.output = {};
        conn.input = {};
    });

    schedule();
}

TEST_CASE("dial - connection refused") {
    RunStats stats;

    spawn([] {
        CHECK_THROWS_AS(net::dial("127.0.0.1", 19999), csp::error);
    });

    schedule();
}

} // TEST_SUITE
