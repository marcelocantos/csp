#include "testutil.h"
#include "testscale.h"

#include <csp/timer.h>

using namespace csp;
using namespace std::chrono_literals;

static Logger g_log("Timer.Test");

TEST_CASE("Timer - Sleep") {
    RunStats stats;

    auto start = clock::now();
    bool ran = false;

    stats.spawn([&]{
        csp::sleep(10ms);
        ran = true;
    });

    csp::schedule();
    CHECK(ran);
    CHECK_GE(clock::now() - start, 10ms);
}

TEST_CASE("Timer - After") {
    RunStats stats;

    auto start = clock::now();
    clock::duration elapsed{};

    stats.spawn([&]{
        auto timer = csp::after(10ms);
        timer.read();
        elapsed = clock::now() - start;
    });

    csp::schedule();
    CHECK_GE(elapsed, 10ms);
}

TEST_CASE("Timer - AfterInAlt") {
    RunStats stats;

    writer<int> idle_writer;
    int which_result = 0;

    stats.spawn([&, r = --idle_writer]{
        auto timeout = csp::after(5ms);
        int n = 0;
        which_result = alt(r >> n, timeout >> poke);
    });

    csp::schedule();
    idle_writer = {};
    CHECK_EQ(2, which_result);
}

TEST_CASE("Timer - Tick") {
    RunStats stats;

    bool ok = true;

    auto interval = CSP_TEST_SANITIZER ? 50ms : 10ms;
    auto threshold = CSP_TEST_SANITIZER ? 40ms : 8ms;

    stats.spawn([&]{
        auto ticker = csp::tick(interval);
        clock::time_point prev = clock::now();
        for (int i = 0; i < 3; ++i) {
            auto tp = ticker.read();
            if (tp - prev < threshold) ok = false;
            prev = tp;
        }
    });

    csp::schedule();
    CHECK(ok);
}

TEST_CASE("Timer - TickCancellation") {
    RunStats stats;

    stats.spawn([]{
        auto ticker = csp::tick(5ms);
        ticker.read();
        ticker = {};
    });

    csp::schedule();
    // If the tick microthread didn't exit, schedule() would hang.
}

TEST_CASE("Timer - MultipleTimersOrdering") {
    RunStats stats;

    int which_result = 0;

    stats.spawn([&]{
        auto slow = csp::after(20ms);
        auto fast = csp::after(5ms);
        which_result = alt(slow >> poke, fast >> poke);
    });

    csp::schedule();
    CHECK_EQ(2, which_result);
}

TEST_CASE("Timer - TimeoutPattern") {
    RunStats stats;

    channel<int> ch;
    int which_result = 0;
    int val = 0;

    stats.spawn([w = +ch]{
        csp::sleep(5ms);
        w << 42;
    });

    stats.spawn([&, r = -ch]{
        auto timeout = csp::after(50ms);
        which_result = alt(r >> val, timeout >> poke);
    });

    ch.release();
    csp::schedule();
    CHECK_EQ(1, which_result);
    CHECK_EQ(42, val);
}
