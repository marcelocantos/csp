#include "testutil.h"

#include <atomic>
#include <stdexcept>
#include <vector>

using namespace csp;

TEST_SUITE("imp_exit") {

TEST_CASE("no interceptor - normal exit") {
    bool ran = false;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            spawn(supervised([&]() { ran = true; }));
        });
    });
    CHECK(ran);
}

TEST_CASE("no interceptor - exception") {
    bool caught = false;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto h = spawn(supervised([]() {
                throw std::runtime_error("boom");
            }));
            std::exception_ptr ep;
            if (h >> ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (std::runtime_error const& e) {
                    caught = (std::string(e.what()) == "boom");
                } catch (...) {}
            }
        });
    });
    CHECK(caught);
}

TEST_CASE("restart on exception") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit(restart_policy{.max_restarts = 3});
            spawn(supervised([&]() {
                ++count;
                if (count == 1) throw std::runtime_error("fail");
            }));
        });
    });
    CHECK(count == 2);
}

TEST_CASE("normal exit not restarted by policy") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit(restart_policy{.max_restarts = 3});
            spawn(supervised([&]() { ++count; }));
        });
    });
    CHECK(count == 1);
}

TEST_CASE("custom handler restarts normal exit") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit([](imp_event ev) {
                if (!ev.error) {
                    ev.restart();
                }
                // exception: drop -> die
            });
            spawn(supervised([&]() {
                ++count;
                if (count >= 3) throw std::runtime_error("stop");
            }));
        });
    });
    CHECK(count == 3);
}

TEST_CASE("max restarts exceeded") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit(restart_policy{.max_restarts = 2});
            spawn(supervised([&count]() {
                ++count;
                throw std::runtime_error("boom");
            }));
        });
    });
    CHECK(count == 3); // initial + 2 restarts
}

TEST_CASE("backoff delay") {
    int count = 0;
    fake_clock fc;
    std::vector<time_point> times;
    RunStats stats;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = on_exit(restart_policy{
            .max_restarts = 3,
            .backoff = std::chrono::seconds(5),
        });
        spawn(supervised([&]() {
            ++count;
            times.push_back(csp::now());
            if (count == 1) throw std::runtime_error("fail");
        }));
    });
    fc.run();
    CHECK(count == 2);
    REQUIRE(times.size() == 2);
    auto delay = times[1] - times[0];
    CHECK(delay >= std::chrono::seconds(5));
}

TEST_CASE("sliding window forgets old restarts") {
    int count = 0;
    fake_clock fc;
    RunStats stats;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = on_exit(restart_policy{
            .max_restarts = 2,
            .window = std::chrono::seconds(10),
        });
        spawn(supervised([&]() {
            ++count;
            fc.advance(std::chrono::seconds(11));
            if (count <= 5) throw std::runtime_error("fail");
        }));
    });
    fc.run();
    // Each restart sees an empty window (11s > 10s window), so never hits max.
    CHECK(count == 6);
}

TEST_CASE("handler drops event") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit([](imp_event ev) {
                // Drop ev without calling restart() -> imp dies.
            });
            spawn(supervised([&]() {
                ++count;
                throw std::runtime_error("fail");
            }));
        });
    });
    CHECK(count == 1);
}

TEST_CASE("nested on_exit scopes") {
    int inner_count = 0;
    int outer_count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto outer = on_exit([&](imp_event ev) {
                ++outer_count;
                // Don't restart.
            });
            {
                auto inner = on_exit([&](imp_event ev) {
                    ++inner_count;
                    // Don't restart.
                });
                spawn(supervised([&]() {
                    throw std::runtime_error("fail");
                }));
            }
            // inner guard destroyed; outer scope now active.
            spawn(supervised([&]() {
                throw std::runtime_error("fail2");
            }));
        });
    });
    CHECK(inner_count == 1);
    CHECK(outer_count == 1);
}

TEST_CASE("inherited by child imps") {
    int handler_count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit([&](imp_event ev) {
                ++handler_count;
                // Don't restart.
            });
            // Spawn a parent that spawns a supervised child.
            spawn([&]() {
                spawn(supervised([&]() {
                    throw std::runtime_error("child fail");
                }));
            });
        });
    });
    CHECK(handler_count == 1);
}

TEST_CASE("channel preservation on restart") {
    int sum = 0;
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            chan<int> ch;

            auto guard = on_exit(restart_policy{.max_restarts = 3});

            // Writer: sends values, crashes once, then finishes.
            spawn(supervised([&, w = ch.w.copy()]() {
                ++count;
                w << count * 10;
                if (count == 1) throw std::runtime_error("fail");
            }));

            // Reader: collect values.
            spawn([&, r = ch.r.copy()]() {
                for (int v; r >> v;) {
                    sum += v;
                }
            });
        });
    });
    // count=1: sends 10, throws, restarted. count=2: sends 20, exits normally.
    // Reader gets 10 + 20 = 30 (channel persists across restart).
    CHECK(count == 2);
    CHECK(sum == 30);
}

TEST_CASE("regular spawn unaffected") {
    bool caught = false;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit([](imp_event ev) {
                ev.restart(); // always restart
            });
            // Regular spawn (not supervised) should NOT be intercepted.
            auto h = spawn([]() {
                throw std::runtime_error("boom");
            });
            std::exception_ptr ep;
            if (h >> ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (std::runtime_error const& e) {
                    caught = (std::string(e.what()) == "boom");
                } catch (...) {}
            }
        });
    });
    CHECK(caught);
}

// TODO(T11): heap-use-after-free under ASan — supervised function
// outlives exit_guard scope in M:N mode. Needs supervisor M:N safety fix.
TEST_CASE("exit_guard RAII cleanup" * doctest::skip()) {
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            {
                auto guard = on_exit([](imp_event ev) {
                    ev.restart();
                });
                // guard goes out of scope — policy imp's channel closes.
            }
            // After guard is gone, supervised spawn should fall through
            // (no interceptor active).
            int count = 0;
            spawn(supervised([&]() { ++count; }));
        });
    });
}

TEST_CASE("channel leak check") {
    int count = 0;
    RunStats stats;
    csp::run([&] {
        stats.spawn([&]() {
            auto guard = on_exit(restart_policy{.max_restarts = 2});
            spawn(supervised([&]() {
                ++count;
                if (count <= 2) throw std::runtime_error("fail");
            }));
        });
    });
    CHECK(count == 3);
}

} // TEST_SUITE("imp_exit")

TEST_SUITE("imp_exit MN") {

TEST_CASE("restart under MN") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);
    std::atomic<int> count{0};
    csp::spawn([&]() {
        auto guard = on_exit(restart_policy{.max_restarts = 3});
        chan<> done;
        spawn(supervised([&, w = std::move(done.w)]() mutable {
            int c = ++count;
            if (c == 1) throw std::runtime_error("fail");
            w = {};  // signal done on successful exit
        }));
        prialt(~done.r);  // wait for supervised imp to finish
    });
    csp::schedule();
    CHECK(count == 2);
    csp::shutdown_runtime();
}

} // TEST_SUITE("imp_exit MN")
