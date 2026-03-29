#include "testutil.h"

using namespace csp;

static Logger g_log("ChanMain.Test");

TEST_CASE("ChanMain - Write") {
    RunStats stats;
    int result = 0;

    csp::run([&]{
        auto [w, r] = chan<int>{};

        stats.spawn([i = std::move(r), &result]{
            i >> result;
        });

        w << 42;
    });

    CHECK(42 == result);
}

TEST_CASE("ChanMain - Read") {
    RunStats stats;
    int result = 0;

    csp::run([&]{
        auto [w, r] = chan<int>{};

        stats.spawn([o = std::move(w)]{
            o << 42;
        });

        result = r.read();
    });

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
            result += n;
        };
        CHECK(15 == result);
    };
};

// First confirm that it runs as a regular imp.
TEST_CASE("ChanMain - WriteReadNormal") {
    RunStats stats;

    csp::run([&]{
        auto ch = chan<int>(5);
        stats.spawn(worker(std::move(ch.w), std::move(ch.r)));
    });
}

// Now try from main (via csp::run).
TEST_CASE("ChanMain - WriteReadFromMain") {
    RunStats stats;

    csp::run([&]{
        auto ch = chan<int>(5);
        auto work = worker(std::move(ch.w), std::move(ch.r));
        work();
    });
}
