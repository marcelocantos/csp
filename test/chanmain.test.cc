#include "testutil.h"

using namespace csp;

static Logger g_log("ChanMain.Test");

TEST_CASE("ChanMain - Write") {
    RunStats stats;

    auto [w, r] = chan<int>{};
    int result = 0;

    stats.spawn([i = std::move(r), &result]{
        i >> result;
    });

    auto o = std::move(w);

    csp::internal::run();
    o << 42;
    csp::internal::run();

    CHECK(42 == result);
}

TEST_CASE("ChanMain - Read") {
    RunStats stats;

    auto [w, r] = chan<int>{};

    stats.spawn([o = std::move(w)]{
        o << 42;
    });

    auto i = std::move(r);

    // Give reader a chance to block on output.
    csp::internal::run();

    int result = i.read();

    // Let reader exit.
    csp::internal::run();

    CHECK(42 == result);
}

auto worker = [](auto && o, auto && i) {
    return [o = std::move(o), i = std::move(i)]() mutable {
        for (int n = 1; n <= 5; ++n) {
            CHECK(bool(o << n));
        }

        o = {};

        int result = 0;
        int n;
        while (i >> n) {
            csp::internal::run();
            result += n;
        };
        CHECK(15 == result);
    };
};

// First confirm that it runs as a regular imp.
TEST_CASE("ChanMain - WriteReadNormal") {
    RunStats stats;

    auto ch = chan<int>(5);
    stats.spawn(worker(std::move(ch.w), std::move(ch.r)));

    while (csp::internal::run()) { }
}

// Now try from main.
TEST_CASE("ChanMain - WriteReadFromMain") {
    RunStats stats;

    auto ch = chan<int>(5);
    auto work = worker(std::move(ch.w), std::move(ch.r));

    csp::internal::run();
    work();
}
