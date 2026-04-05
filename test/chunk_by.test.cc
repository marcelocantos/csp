#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("ChunkBy---consecutive-equal-groups") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | chunk_by<int>([](int a, int b) { return a == b; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 1; w << 2; w << 2; w << 2; w << 3; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{1, 1}, {2, 2, 2}, {3}});
}

TEST_CASE("ChunkBy---monotone-runs") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | chunk_by<int>([](int a, int b) { return b > a; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 3; w << 5; w << 2; w << 4; w << 1; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{1, 3, 5}, {2, 4}, {1}});
}

TEST_CASE("ChunkBy---single-element") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | chunk_by<int>([](int, int) { return false; });

    stats.spawn([w = std::move(src.w)]{ w << 42; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{42}});
}

TEST_CASE("ChunkBy---empty-input") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | chunk_by<int>([](int, int) { return true; });

    stats.spawn([w = std::move(src.w)]{ });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got.empty());
}

TEST_CASE("ChunkBy---all-same-group") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | chunk_by<int>([](int, int) { return true; });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{1, 2, 3}});
}
