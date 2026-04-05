#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("TakeUntil---includes-terminating-element") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | take_until<int>([](int n) { return n == 3; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; w << 4; w << 5; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK(got == std::vector<int>{1, 2, 3});
}

TEST_CASE("TakeUntil---predicate-never-true") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | take_until<int>([](int) { return false; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK(got == std::vector<int>{1, 2, 3});
}

TEST_CASE("TakeUntil---first-element-matches") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | take_until<int>([](int n) { return n > 0; });

    stats.spawn([w = std::move(src.w)]{ w << 5; w << 10; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK(got == std::vector<int>{5});
}

TEST_CASE("TakeUntil---empty-input") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r) | take_until<int>([](int) { return true; });

    stats.spawn([w = std::move(src.w)]{ });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK(got.empty());
}
