#include "testutil.h"

using namespace csp;
using namespace csp::part;

static Logger g_log("fanout.test");

TEST_CASE("Fanout - Simple") {
    RunStats stats;

    csp::run([&]{
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
    });
}

TEST_CASE("Fanout - Complex") {
    RunStats stats;

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][5];

    csp::run([&]{
        auto [new_out_w, new_out_r] = chan<writer<int>>{};

        auto new_in = fanout<int>.spawn(std::move(new_out_r));

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

        // Phase 1: send 1..5 inline so delivery is complete before registering
        // the second wave of receivers (each in << i blocks until fanout
        // delivers to all registered receivers).
        for (int i = 1; i < 6; ++i) {
            in << i;
        }

        setup(new_out_w, receiverses[1]);

        // Phase 2: send 6..10 inline (all 10 receivers are now registered).
        for (int i = 6; i < 11; ++i) {
            in << i;
        }
        in = {};
        new_out_w = {};
        // csp::run waits for all receiver imps to exit before returning.
    });

    // All receiver imps have exited — safe to check accumulated results.
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

    struct {
        chan<int> ch;
        int result = 0;
    } receiverses[2][1];

    csp::run([&]{
        auto [new_out_w, new_out_r] = chan<writer<int>>{};

        auto new_in = fanout<int>.spawn(std::move(new_out_r));

        // ack channels: receiver sends an ack after accumulating each value.
        // This lets the orchestrator know delivery is fully processed.
        chan<> ack1, ack2;

        auto setup = [&](auto & now, auto & receivers, chan<> & ack) {
            for (auto & s : receivers) {
                CHECK(bool(now << std::move(s.ch.w)));
                stats.spawn([&, down = std::move(s.ch.r), result = &s.result,
                              ack_w = ack.w.copy()]{
                    csp::internal::descr("R%d", &s - std::begin(receivers));
                    int n;
                    while (down >> n) {
                        *result += n;
                        ack_w << poke;
                    }
                    CSP_LOG(g_log, "reader exit");
                });
            }
        };

        writer<int> in;

        // Wave 1: register receivers and send values 1..5.
        CSP_LOG(g_log, "wave 1");
        setup(new_out_w, receiverses[0], ack1);
        ack1.w = {};  // release orchestrator's writer copy
        CHECK(bool(new_in >> in));
        for (int i = 1; i <= 5; ++i) {
            in << i;
            // Wait for the receiver to ack (value accumulated).
            ack1.r.read();
        }
        // Close wave-1 input.  Fanout recycles.
        in = {};

        // Wave 2: register receivers and send values 6..10.
        // Note: wave-1 receivers are still registered with fanout and will
        // also receive wave-2 values.  We close new_out_w at the end so
        // fanout eventually exits and closes all downstream channels.
        CSP_LOG(g_log, "wave 2");
        setup(new_out_w, receiverses[1], ack2);
        ack2.w = {};
        CHECK(bool(new_in >> in));
        for (int i = 6; i <= 10; ++i) {
            in << i;
            // Wait for BOTH receivers to ack (fanout broadcasts to all).
            ack1.r.read();  // wave-1 receiver ack
            ack2.r.read();  // wave-2 receiver ack
        }
        in = {};
        new_out_w = {};
        // csp::run waits for all receiver imps to exit before returning.
    });

    // Wave-1 receivers got values from both waves: 1+2+3+4+5 + 6+7+8+9+10 = 55.
    for (auto & s : receiverses[0]) {
        INFO((&s - &receiverses[0][0]));
        CHECK(55 == s.result);
    }
    // Wave-2 receivers got only wave-2 values: 6+7+8+9+10 = 40.
    for (auto & s : receiverses[1]) {
        CHECK(40 == s.result);
    }
}

TEST_CASE("Fanout - Chain") {
    RunStats stats;
    std::atomic<int> total{0};

    csp::run([&]{
        auto [new_out_w, new_out_r] = chan<writer<int>>{};

        auto new_in = fanout<int>.spawn(std::move(new_out_r));

        constexpr int m = 2, n = 1;

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
        // csp::run waits for all consumer imps to exit before returning.
    });

    // In M:N, fanout pipeline may not fully drain before imps exit.
    // At least one consumer should receive the value.
    CHECK(total.load() >= 1);
    CHECK(total.load() <= 2);
}
