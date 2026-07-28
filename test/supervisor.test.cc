#include "testutil.h"

#include <atomic>
#include <stdexcept>

using namespace csp;

TEST_SUITE("worker_group") {

TEST_CASE("all-workers-exit-normally") {
    RunStats stats;
    int a = 0, b = 0, c = 0;
    stats.spawn([&]() {
        worker_group wg;
        wg.workers = {
            {"a", [&]() { a = 1; }},
            {"b", [&]() { b = 2; }},
            {"c", [&]() { c = 3; }},
        };
        wg.run();
    });
    csp::await_completion();
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
}

TEST_CASE("worker-exception-triggers-restart") {
    RunStats stats;
    int count = 0;
    stats.spawn([&]() {
        worker_group wg;
        wg.workers["flaky"] = [&]() {
            ++count;
            if (count == 1) throw std::runtime_error("fail");
        };
        wg.run();
    });
    csp::await_completion();
    CHECK(count == 2);
}

TEST_CASE("max-restarts-exceeded") {
    RunStats stats;
    int count = 0;
    bool caught = false;
    stats.spawn([&]() {
        worker_group wg;
        wg.policy.max_restarts = 2;
        wg.workers["always_fail"] = [&count]() {
            ++count;
            throw std::runtime_error("boom");
        };
        try {
            wg.run();
        } catch (worker_max_restarts_exceeded const& e) {
            caught = true;
            CHECK(e.worker_name == "always_fail");
            CHECK(e.cause != nullptr);
        }
    });
    csp::await_completion();
    CHECK(caught);
    CHECK(count == 3); // initial + 2 restarts
}

TEST_CASE("sliding-window-forgets-old-restarts") {
    int count = 0;
    fake_clock fc;
    RunStats stats;
    csp::run([&] {
        csp::local l{fc.binding()};
        // max_restarts = 2, window = 10s.
        // Worker advances fake time by 11s before each throw,
        // pushing prior restarts outside the window.
        stats.spawn([&]() {
            worker_group wg;
            wg.policy.max_restarts = 2;
            wg.policy.window = std::chrono::seconds(10);
            wg.workers["timed_fail"] = [&count, &fc]() {
                ++count;
                fc.advance(std::chrono::seconds(11));
                if (count <= 5) throw std::runtime_error("fail");
            };
            wg.run();
        });
    });
    // Without window expiry, max_restarts=2 would escalate at count=3.
    // With expiry, each restart sees an empty window: 5 failures + 1 success.
    CHECK(count == 6);
}

TEST_CASE("backoff-delays-restart") {
    int count = 0;
    fake_clock fc;
    std::vector<time_point> times;
    RunStats stats;
    csp::run([&] {
        csp::local l{fc.binding()};
        stats.spawn([&]() {
            worker_group wg;
            wg.policy.max_restarts = 3;
            wg.policy.backoff = std::chrono::seconds(5);
            wg.workers["fail_once"] = [&]() {
                ++count;
                times.push_back(csp::now());
                if (count == 1) throw std::runtime_error("fail");
            };
            wg.run();
        });
    });
    CHECK(count == 2);
    REQUIRE(times.size() == 2);
    auto delay = times[1] - times[0];
    CHECK(delay >= std::chrono::seconds(5));
}

TEST_CASE("mixed-workers") {
    RunStats stats;
    int stable_count = 0, flaky_count = 0;
    stats.spawn([&]() {
        worker_group wg;
        wg.workers["stable"] = [&]() { ++stable_count; };
        wg.workers["flaky"] = [&]() {
            ++flaky_count;
            if (flaky_count == 1) throw std::runtime_error("fail");
        };
        wg.run();
    });
    csp::await_completion();
    CHECK(stable_count == 1);
    CHECK(flaky_count == 2);
}

TEST_CASE("nested-groups") {
    RunStats stats;
    bool caught = false;
    stats.spawn([&]() {
        worker_group inner;
        inner.policy.max_restarts = 1;
        inner.workers["bad"] = []() {
            throw std::runtime_error("inner fail");
        };

        worker_group outer;
        outer.policy.max_restarts = 0;
        outer.workers["inner_group"] = std::move(inner);

        try {
            outer.run();
        } catch (worker_max_restarts_exceeded const& e) {
            caught = true;
            CHECK(e.worker_name == "inner_group");
        }
    });
    csp::await_completion();
    CHECK(caught);
}

TEST_CASE("empty-group") {
    RunStats stats;
    stats.spawn([&]() {
        worker_group wg;
        wg.run(); // should return immediately
    });
    csp::await_completion();
}

TEST_CASE("channel-leak-check") {
    // RunStats destructor checks channel_count == 0.
    RunStats stats;
    int count = 0;
    stats.spawn([&]() {
        worker_group wg;
        wg.policy.max_restarts = 2;
        wg.workers["leaky"] = [&count]() {
            ++count;
            if (count <= 2) throw std::runtime_error("fail");
        };
        wg.run();
    });
    csp::await_completion();
    CHECK(count == 3);
}

} // TEST_SUITE("worker_group")

TEST_SUITE("worker_group MN") {

TEST_CASE("concurrent-workers") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);
    std::atomic<int> sum{0};
    csp::spawn([&]() {
        worker_group wg;
        for (int i = 0; i < 10; ++i) {
            wg.workers["w" + std::to_string(i)] = [&sum, i]() {
                sum += i;
            };
        }
        wg.run();
    });

    csp::await_completion();
    CHECK(sum == 45);
    csp::shutdown_runtime();
}

TEST_CASE("restart-under-contention") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);
    std::atomic<int> fail_count{0};
    std::atomic<int> total{0};
    csp::spawn([&]() {
        worker_group wg;
        wg.policy.max_restarts = 3;
        wg.workers["steady"] = [&total]() {
            for (int i = 0; i < 100; ++i) {
                total++;
                csp::yield();
            }
        };
        wg.workers["flaky"] = [&fail_count, &total]() {
            total++;
            if (++fail_count == 1) throw std::runtime_error("fail");
        };
        wg.run();
    });

    csp::await_completion();
    CHECK(total == 102); // 100 from steady + 2 from flaky (fail + success)
    csp::shutdown_runtime();
}

} // TEST_SUITE("worker_group MN")
