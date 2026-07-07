#include "testutil.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

TEST_CASE("Thread---OneShot") {
    bool ran = false;
    csp::run([&]{
        csp::spawn([&]{
            ran = true;
        });
    });
    CHECK(ran);
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Thread---Parallel") {
    char buf[6] = "";
    csp::run([&]{
        for (int i = 0; i < 5; ++i) {
            csp::spawn([&, i]{
                buf[i] = static_cast<char>(478560413032U >> (8 * i));
            });
        }
    });
    CHECK("hello" == std::string(buf));
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Thread---SpawnSpawn") {
    // Atomic: the 25 grandchild imps run concurrently on multiple OS
    // threads under M:N; a plain int loses increments (seen 1/40 runs,
    // dist-notls).
    std::atomic<int> result{0};
    csp::run([&]{
        for (int i = 0; i < 5; ++i) {
            csp::spawn([&]{
                for (int i = 0; i < 5; ++i) {
                    csp::spawn([&]{
                        result += 1;
                    });
                }
            });
        }
    });
    CHECK(25 == result);
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Thread---Throw") {
    struct bork { };

    int total = 0;
    bool threw = false;
    csp::run([&]{
        // The throwing imp blocks on channel send until joined.
        // Both the thrower and the joiner must live in the same csp::run
        // so the channel rendezvous can complete within await_completion().
        auto ex = csp::spawn([&]{
            for (int i = 1; i <= 10; ++i) {
                total += i;
                if (i == 5) {
                    throw bork{};
                }
            }
        });
        csp::spawn([ex = std::move(ex), &threw]{
            CHECK_THROWS_AS(csp::join(ex), bork);
            threw = true;
        });
    });

    CHECK(threw);
    CHECK(15 == total);  // 1+2+3+4+5
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Thread---Yield") {
    // The two imps run on different OS threads under M:N; appends to a
    // shared std::string race (lost appends seen ~1/40 runs,
    // dist-notls). Serialize them; the assertions below only check
    // completion counts, not order.
    std::string trace;
    std::mutex trace_mu;
    auto app = [&](char c) {
        std::lock_guard<std::mutex> lk(trace_mu);
        trace += c;
    };

    csp::run([&]{
        csp::spawn([&]{
            app('A');
            csp::yield();
            app('A');
        });
        csp::spawn([&]{
            app('B');
            csp::yield();
            app('B');
        });
    });

    CHECK(2 == std::count(trace.begin(), trace.end(), 'A'));
    CHECK(2 == std::count(trace.begin(), trace.end(), 'B'));
    // In M:N mode imps run in parallel, so ordering is non-deterministic.
    // Just verify both imps completed — all characters must appear.
    CHECK(4 == trace.size());
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Thread---CustomScheduler") {
    // Verify that set_scheduler installs a custom scheduler that gets called.
    bool custom_ran = false;

    csp::set_scheduler([&]{
        custom_ran = true;
        // Drain any pending imps so the runtime reaches a clean state.
        while (csp::internal::run()) { }
    });

    csp::spawn([]{});
    csp::schedule();

    CHECK(custom_ran);

    // Reset to default.
    csp::reset_scheduler();
}

TEST_CASE("Thread---SpawnMany") {
    constexpr int N = 500;
    std::atomic<int> completed{0};

    csp::run([&]{
        for (int i = 0; i < N; ++i) {
            csp::spawn([&]{ ++completed; });
        }
    });

    CHECK(N == completed.load());
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}
