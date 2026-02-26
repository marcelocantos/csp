#include "testutil.h"

using namespace csp;
using namespace csp::part;

static Logger g_log("fanout.test");

TEST_CASE("Fanout - Simple") {
    RunStats stats;

    CSP_LOG(g_log, "new_out{}");
    auto [new_out_w, new_out_r] = chan<writer<int>>{};
    CSP_LOG(g_log, "spawn_fanout");
    auto new_in = fanout<int>.spawn(std::move(new_out_r));
    CSP_LOG(g_log, "out{}");
    auto [out_w, out_r] = chan<int>{};

    CSP_LOG(g_log, "new_out << std::move(out_w)");
    CHECK(bool(new_out_w << std::move(out_w)));

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

    CSP_LOG(g_log, "out_r.read()");
    CHECK(42 == out_r.read());

    CSP_LOG(g_log, "EOT");
}

TEST_CASE("Fanout - Complex") {
    RunStats stats;

    auto [new_out_w, new_out_r] = chan<writer<int>>{};

    auto new_in = fanout<int>.spawn(std::move(new_out_r));

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][5];

    auto setup = [&](auto & now, auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(now << std::move(s.ch.w)));
            stats.spawn([&, down = std::move(s.ch.r), result = &s.result]{
                int n;
                while (down >> n) {
                    *result += n;
                }
            });
        }
    };

    // Start with 5 receivers to receive 1..5; grow to 10 receiving 6..10.
    setup(new_out_w, receiverses[0]);

    writer<int> in;
    CHECK(bool(new_in >> in));
    new_in = {};

    stats.spawn(count(1, 6).bind(in.copy()));
    schedule();

    setup(new_out_w, receiverses[1]);
    stats.spawn(count(6, 11).bind(std::move(in)));

    schedule();

    for (auto & s : receiverses[0]) {
        INFO((&s - &receiverses[0][0]));
        CHECK(55 == s.result);
    }
    for (auto & s : receiverses[1]) {
        CHECK(40 == s.result);
    }
}

TEST_CASE("Fanout - Waves") {
    RunStats stats;

    auto [new_out_w, new_out_r] = chan<writer<int>>{};
    chan<> keepalive;

    auto new_in = fanout<int>.spawn(std::move(new_out_r));

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][1];

    auto setup = [&](auto & now, auto & receivers) {
        for (auto & s : receivers) {
            CHECK(bool(now << std::move(s.ch.w)));
            stats.spawn([&, down = std::move(s.ch.r), result = &s.result, keepalive = keepalive.w.copy()]{
                csp::internal::descr("R%d", &s - std::begin(receivers));
                int n;
                while (alt(down >> n, ~keepalive) >= 0) {
                    *result += n;
                }
                CSP_LOG(g_log, "reader exit");
            });
        }
    };

    writer<int> in;

    // Start with 5 receivers to receive 1..5; grow to 10 receiving 6..10.
    CSP_LOG(g_log, "wave 1");
    setup(new_out_w, receiverses[0]);

    CHECK(bool(new_in >> in));

    for (int i = 1; i <= 5; ++i) {
        in << i;
    }
    keepalive = {};

    CSP_LOG(g_log, "wait for ~in");
    CHECK_FALSE(~in);

    CSP_LOG(g_log, "wave 2");
    setup(new_out_w, receiverses[1]);

    CHECK(bool(new_in >> in));

    for (int i = 6; i <= 10; ++i) {
        in << i;
    }
    keepalive = {};
    schedule();

    for (auto & s : receiverses[0]) {
        INFO((&s - &receiverses[0][0]));
        CHECK(15 == s.result);
    }
    for (auto & s : receiverses[1]) {
        CHECK(40 == s.result);
    }
}

TEST_CASE("Fanout - Chain") {
    RunStats stats;

    auto [new_out_w, new_out_r] = chan<writer<int>>{};

    auto new_in = fanout<int>.spawn(std::move(new_out_r));

    constexpr int m = 2, n = 1;
    int total = 0;

    for (int i = 0; i < m; ++i) {
        auto [new_out2_w, new_out2_r] = chan<writer<int>>{};
        stats.spawn(fanout<int>.bind(std::move(new_out2_r), new_out_w.copy()));

        for (int j = 0; j < n; ++j) {
            new_out2_w << spawn_consumer<int>([&](auto r) {
                csp::internal::descr("fanout");
                BRAC_SCOPE(g_log, "FanoutChain::λ", "%d, %d", i, j);

                for (int i; r >> i;) {
                    CSP_LOG(g_log, "received %d", i);
                    total += i;
                }
            });
        }
    }
    new_out_w = {};

    writer<int> in;
    new_in >> in;
    new_in = {};

    in << 1;
    in = {};

    csp::schedule();
    CHECK(total == m * n);
}
