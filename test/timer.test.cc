#include "testutil.h"

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

static Logger g_log("Timer.Test");

TEST_CASE("Timer---Sleep") {
    RunStats stats;
    fake_clock fc;
    bool ran = false;

    csp::run([&] {
        csp::local l{fc.binding()};
        csp::sleep(10ms);
        ran = true;
    });

    CHECK(ran);
}

TEST_CASE("Timer---After") {
    RunStats stats;
    fake_clock fc;
    duration elapsed{};

    csp::run([&] {
        csp::local l{fc.binding()};
        auto start = csp::now();
        auto timer = csp::after(10ms);
        timer.read();
        elapsed = csp::now() - start;
    });

    CHECK(elapsed == 10ms);
}

TEST_CASE("Timer---AfterInAlt") {
    RunStats stats;
    fake_clock fc;

    writer<int> idle_writer;
    int which_result = 0;

    csp::run([&, r = --idle_writer] {
        csp::local l{fc.binding()};
        auto timeout = csp::after(5ms);
        int n = 0;
        which_result = alt(r >> n, timeout >> nullptr);
    });

    idle_writer = {};
    CHECK(1 == which_result);
}

TEST_CASE("Timer---Tick") {
    RunStats stats;
    fake_clock fc;
    auto epoch = time_point{};

    std::vector<time_point> times;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto ticker = csp::tick(50ms);
        for (int i = 0; i < 3; ++i) {
            times.push_back(ticker.read());
        }
    });

    REQUIRE(3u == times.size());
    CHECK(times[0] == epoch + 50ms);
    CHECK(times[1] == epoch + 100ms);
    CHECK(times[2] == epoch + 150ms);
}

TEST_CASE("Timer---TickCancellation") {
    RunStats stats;
    fake_clock fc;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto ticker = csp::tick(5ms);
        ticker.read();
        ticker = {};
    });

    // If the tick imp didn't exit, await_completion() would hang.
}

TEST_CASE("Timer---MultipleTimersOrdering") {
    RunStats stats;
    fake_clock fc;

    int which_result = -1;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto slow = csp::after(20ms);
        auto fast = csp::after(5ms);
        // The alt must run in a scope PARTICIPANT (a child imp inherits
        // the quiescence scope): the binding imp is the orchestrator and
        // not a member, so if it alt-ed directly, fake time could advance
        // past BOTH deadlines before the alt registers — and alt picks
        // randomly between two ready arms (latent flake exposed by
        // 🎯T34's scheduling changes).
        join(spawn([&which_result, slow = std::move(slow),
                    fast = std::move(fast)]() mutable {
            which_result = alt(slow >> nullptr, fast >> nullptr);
        }));
    });

    CHECK(1 == which_result);
}

TEST_CASE("Timer---TimeoutPattern") {
    RunStats stats;
    fake_clock fc;

    int which_result = 0;
    int val = 0;

    csp::run([&] {
        csp::local l{fc.binding()};
        chan<int> ch;

        csp::spawn([w = ch.w.copy()] {
            csp::sleep(5ms);
            w << 42;
        });

        csp::spawn([&, r = ch.r.copy()] {
            auto timeout = csp::after(50ms);
            which_result = alt(r >> val, timeout >> nullptr);
        });

        ch.release();
    });

    CHECK(0 == which_result);
    CHECK(42 == val);
}

TEST_CASE("Timer---Controlled-duration") {
    RunStats stats;
    fake_clock fc;
    std::vector<duration> intervals;

    csp::run([&] {
        csp::local l{fc.binding()};
        chan<duration> ctl;
        auto t = timer(std::move(ctl.r));

        csp::spawn([w = std::move(ctl.w)]() mutable {
            w << 10ms; w << 20ms; w << 10ms;
        });

        time_point prev = csp::now();
        for (time_point tp; t >> tp;) {
            intervals.push_back(tp - prev);
            prev = tp;
        }
    });

    REQUIRE(3 == intervals.size());
    CHECK(intervals[0] == 10ms);
    CHECK(intervals[1] == 20ms);
    CHECK(intervals[2] == 10ms);
}

TEST_CASE("Timer---Controlled-time_point") {
    RunStats stats;
    fake_clock fc;
    int fires = 0;

    csp::run([&] {
        csp::local l{fc.binding()};
        chan<time_point> ctl;
        auto t = timer(std::move(ctl.r));

        auto now = csp::now();
        csp::spawn([w = std::move(ctl.w), now]() mutable {
            w << now + 10ms;
            w << now + 30ms;
        });

        for (; t >> nullptr;) fires++;
    });

    CHECK(2 == fires);
}

TEST_CASE("Timer---Controlled-cancellation") {
    RunStats stats;
    fake_clock fc;

    csp::run([&] {
        csp::local l{fc.binding()};
        chan<duration> ctl;
        auto t = timer(std::move(ctl.r));

        csp::spawn([w = std::move(ctl.w)]() mutable {
            w << 5ms;
        });

        t.read();
        t = {};
    });
}
