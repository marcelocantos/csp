#include "testutil.h"

#include <csp/count.h>
#include <csp/fanout.h>

using namespace csp;

static Logger g_log("fanout.test");

TEST_CASE("Fanout - Simple") {
    RunStats stats;

    CSP_LOG(g_log, "new_out{}");
    chan<writer<int>> new_out;
    CSP_LOG(g_log, "spawn_fanout");
    auto new_in = spawn_fanout(std::move(new_out.r));
    CSP_LOG(g_log, "out{}");
    chan<int> out;

    CSP_LOG(g_log, "new_out << std::move(out.w)");
    CHECK(bool(new_out.w << std::move(out.w)));

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

    CSP_LOG(g_log, "out.r.read()");
    CHECK_EQ(42, out.r.read());

    CSP_LOG(g_log, "EOT");
}

TEST_CASE("Fanout - Complex") {
    RunStats stats;

    chan<writer<int>> new_out;

    auto new_in = spawn_fanout(std::move(new_out.r));

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][5];

    auto setup = [&](auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(new_out.w << std::move(s.ch.w)));
            stats.spawn([&, down = std::move(s.ch.r), result = &s.result]{
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

    stats.spawn(count(in.copy(), 1, 6));
    schedule();

    setup(receiverses[1]);
    stats.spawn(count(std::move(in), 6, 11));

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
    RunStats stats;

    chan<writer<int>> new_out;
    chan<> keepalive;

    auto new_in = spawn_fanout(std::move(new_out.r));

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][1];

    auto setup = [&](auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(new_out.w << std::move(s.ch.w)));
            stats.spawn([&, down = std::move(s.ch.r), result = &s.result, keepalive = keepalive.w.copy()]{
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

    chan<writer<int>> new_out;

    auto new_in = spawn_fanout(std::move(new_out.r));

    constexpr int m = 2, n = 1;
    int total = 0;

    for (int i = 0; i < m; ++i) {
        chan<writer<int>> new_out2;
        stats.spawn(fanout(std::move(new_out2.r), new_out.w.copy()));

        for (int j = 0; j < n; ++j) {
            new_out2.w << spawn_consumer<int>([&](auto r) {
                csp_descr("fanout");
                BRAC_SCOPE(g_log, "FanoutChain::λ", "%d, %d", i, j);

                for (int i; r >> i;) {
                    CSP_LOG(g_log, "received %d", i);
                    total += i;
                }
            });
        }
    }
    new_out.w = {};

    writer<int> in;
    new_in >> in;
    new_in = {};

    in << 1;
    in = {};

    csp::schedule();
    CHECK_EQ(total, m * n);
}
