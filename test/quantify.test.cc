#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("AnyOf - match found") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | any_of<int>([](int n) { return n > 3; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 4; w << 5; });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), true);
    });
    csp::schedule();
}

TEST_CASE("AnyOf - no match") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | any_of<int>([](int n) { return n > 10; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), false);
    });
    csp::schedule();
}

TEST_CASE("AnyOf - empty input") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | any_of<int>([](int) { return true; });

    stats.spawn([w = std::move(src.w)]{ });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), false);
    });
    csp::schedule();
}

TEST_CASE("AllOf - all match") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | all_of<int>([](int n) { return n > 0; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), true);
    });
    csp::schedule();
}

TEST_CASE("AllOf - early mismatch") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | all_of<int>([](int n) { return n > 0; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << -1; w << 2; w << 3; });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), false);
    });
    csp::schedule();
}

TEST_CASE("AllOf - empty input") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | all_of<int>([](int) { return false; });

    stats.spawn([w = std::move(src.w)]{ });
    stats.spawn([r = std::move(out)] {
        CHECK_EQ(r.read(), true);
    });
    csp::schedule();
}
