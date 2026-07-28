#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("SortMerge---three-sorted-streams") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();
    auto c = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    inputs.push_back(std::move(c.r));
    auto out = sort_merge(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 1; w << 4; w << 7; });
    stats.spawn([w = std::move(b.w)]{ w << 2; w << 5; w << 8; });
    stats.spawn([w = std::move(c.w)]{ w << 3; w << 6; w << 9; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9});
}

TEST_CASE("SortMerge---interleaved-values") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = sort_merge(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 1; w << 3; w << 5; });
    stats.spawn([w = std::move(b.w)]{ w << 2; w << 4; w << 6; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got == std::vector<int>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("SortMerge---one-empty-stream") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = sort_merge(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ });
    stats.spawn([w = std::move(b.w)]{ w << 1; w << 2; w << 3; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got == std::vector<int>{1, 2, 3});
}

TEST_CASE("SortMerge---custom-comparator-(descending)") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = sort_merge(std::move(inputs), std::greater<int>{}).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 9; w << 6; w << 3; });
    stats.spawn([w = std::move(b.w)]{ w << 8; w << 5; w << 2; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got == std::vector<int>{9, 8, 6, 5, 3, 2});
}

TEST_CASE("SortMerge---single-stream") {
    RunStats stats;
    auto a = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    auto out = sort_merge(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 10; w << 20; w << 30; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got == std::vector<int>{10, 20, 30});
}
