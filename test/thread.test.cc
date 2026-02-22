#include "testutil.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <stdexcept>
#include <string>

TEST_CASE("Thread - OneShot") {
    bool ran = false;
    csp::spawn([&]{
        ran = true;
    });

    CHECK_FALSE(csp::internal::run());
    CHECK(ran);
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}

TEST_CASE("Thread - Parallel") {
    char buf[6] = "";
    for (int i = 0; i < 5; ++i) {
        csp::spawn([&, i]{
            buf[i] = 478560413032U >> (8 * i);
        });
    }

    CHECK_FALSE(csp::internal::run());
    CHECK_EQ("hello", std::string(buf));
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}

TEST_CASE("Thread - SpawnSpawn") {
    int result = 0;
    for (int i = 0; i < 5; ++i) {
        csp::spawn([&]{
            for (int i = 0; i < 5; ++i) {
                csp::spawn([&]{
                    result += 1;
                });
            }
        });
    }

    while (csp::internal::run()) { }

    CHECK_EQ(25, result);
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}

TEST_CASE("Thread - Throw") {
    struct bork { };

    int total = 0;
    auto ex = csp::spawn([&]{
        for (int i = 1; i <= 10; ++i) {
            total += i;
            if (i == 5) {
                throw bork{};
            }
        }
    });

    while (csp::internal::run()) { }

    CHECK_EQ(1, csp::internal::channel_count(0));
    CHECK_EQ(1, csp::internal::channel_count(1));
    CHECK_THROWS_AS(csp::join(ex), bork);
    ex = {};
    csp::internal::run();
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}

TEST_CASE("Thread - Yield") {
    std::string trace;

    csp::spawn([&]{
        trace += 'A';
        csp::yield();
        trace += 'A';
    });
    csp::spawn([&]{
        trace += 'B';
        csp::yield();
        trace += 'B';
    });

    while (csp::internal::run()) { }

    CHECK_EQ(2, std::count(trace.begin(), trace.end(), 'A'));
    CHECK_EQ(2, std::count(trace.begin(), trace.end(), 'B'));
    // Yield should cause interleaving, not sequential execution.
    CHECK_NE(std::string("AABB"), trace);
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}

TEST_CASE("Thread - CustomScheduler") {
    bool custom_ran = false;

    csp::set_scheduler([&]{
        custom_ran = true;
        while (csp::internal::run()) { }
    });

    csp::spawn([]{});

    csp::schedule();

    CHECK(custom_ran);

    // Reset to default.
    csp::reset_scheduler();
}

TEST_CASE("Thread - SpawnMany") {
    constexpr int N = 500;
    int completed = 0;

    for (int i = 0; i < N; ++i) {
        csp::spawn([&]{ ++completed; });
    }

    while (csp::internal::run()) { }

    CHECK_EQ(N, completed);
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));
}
