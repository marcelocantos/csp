#include "testutil.h"

#include <csp/count.h>
#include <csp/fanout.h>

using namespace csp;

static Logger g_log("fanout.test");

TEST_CASE("Fanout - Simple") {
    RunStats stats;

    CSP_LOG(g_log, "new_out{}");
    writer<writer<int>> new_out;
    CSP_LOG(g_log, "spawn_fanout");
    auto new_in = chan::spawn_fanout(--new_out);
    CSP_LOG(g_log, "out{}");
    reader<int> out;

    CSP_LOG(g_log, "new_out << ++out");
    CHECK(bool(new_out << ++out));

    CSP_LOG(g_log, "in{}");
    writer<int> in;
    CSP_LOG(g_log, "new_in >> in");
    CHECK(bool(new_in >> in));
    CSP_LOG(g_log, "new_in = {}");
    new_in = {};

    CSP_LOG(g_log, "in << 42");
    in << 42;
    CSP_LOG(g_log, "in = {}");
    in = {};

    CSP_LOG(g_log, "out.read()");
    CHECK_EQ(42, out.read());

    CSP_LOG(g_log, "EOT");
}

TEST_CASE("Fanout - Complex") {
    RunStats stats;

    writer<writer<int>> new_out;

    auto new_in = chan::spawn_fanout(--new_out);

    struct {
        channel<int> ch;
        int result = 0;
    } receiverses[2][5];

    auto setup = [&](auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(new_out << ++s.ch));
            stats.spawn([&, down = --s.ch, result = &s.result]{
                int n;
                while (down >> n) {
                    *result += n;
                }
            });
        }
    };

    // Start with 5 receivers to receive 1..5; grow to 10 receiving 6..10.
    setup(receiverses[0]);

    writer<int> in;
    CHECK(bool(new_in >> in));
    new_in = {};

    stats.spawn(chan::count(in, 1, 6));
    schedule();

    setup(receiverses[1]);
    stats.spawn(chan::count(in, 6, 11));

    in = {};

    schedule();

    for (auto & s : receiverses[0]) {
        INFO((&s - &receiverses[0][0]));
        CHECK_EQ(55, s.result);
    }
    for (auto & s : receiverses[1]) {
        CHECK_EQ(40, s.result);
    }
}

TEST_CASE("Fanout - Waves") {
    // TODO: Test fanout losing all its readers, then acquiring more readers.
    RunStats stats;

    writer<writer<int>> new_out;
    channel<> keepalive;

    auto new_in = chan::spawn_fanout(--new_out);

    struct {
        channel<int> ch;
        int result = 0;
    } receiverses[2][1];

    auto setup = [&](auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(new_out << ++s.ch));
            stats.spawn([&, down = --s.ch, result = &s.result, keepalive = +keepalive]{
                csp_descr("R%d", &s - std::begin(receivers));
                int n;
                while (alt(down >> n, ~keepalive) > 0) {
                    *result += n;
                }
                CSP_LOG(g_log, "reader exit");
            });
        }
    };

    writer<int> in;

    // Start with 5 receivers to receive 1..5; grow to 10 receiving 6..10.
    CSP_LOG(g_log, "wave 1");
    setup(receiverses[0]);

    CHECK(bool(new_in >> in));

    for (int i = 1; i <= 5; ++i) {
        in << i;
    }
    keepalive = {};

    CSP_LOG(g_log, "wait for ~in");
    CHECK_FALSE(~in);

    CSP_LOG(g_log, "wave 2");
    setup(receiverses[1]);

    CHECK(bool(new_in >> in));

    for (int i = 6; i <= 10; ++i) {
        in << i;
    }
    keepalive = {};
    schedule();

    for (auto & s : receiverses[0]) {
        INFO((&s - &receiverses[0][0]));
        CHECK_EQ(15, s.result);
    }
    for (auto & s : receiverses[1]) {
        CHECK_EQ(40, s.result);
    }
}

TEST_CASE("Fanout - Chain") {
    RunStats stats;

    writer<writer<int>> new_out;

    auto new_in = chan::spawn_fanout(--new_out);

    constexpr int m = 2, n = 1;
    int total = 0;

    for (int i = 0; i < m; ++i) {
        writer<writer<int>> new_out2;
        stats.spawn(chan::fanout(--new_out2, new_out));

        for (int j = 0; j < n; ++j) {
            new_out2 << spawn_consumer<int>([&](auto r) {
                csp_descr("chan::fanout");
                BRAC_SCOPE(g_log, "FanoutChain::λ", "%d, %d", i, j);

                for (int i; r >> i;) {
                    CSP_LOG(g_log, "received %d", i);
                    total += i;
                }
            });
        }
    }
    new_out = {};

    writer<int> in;
    new_in >> in;
    new_in = {};

    in << 1;
    in = {};

    csp::schedule();
    CHECK_EQ(total, m * n);
}
