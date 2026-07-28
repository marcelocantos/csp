#include "testutil.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_SUITE("ParallelMap") {

TEST_CASE("Unordered---all-values-processed") {
    RunStats stats;

    auto r = count(1, 6).spawn()
           | parallel_map<int>(4, [](int n) { return n * 10; });

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::await_completion();
    // All values present (order may vary).
    std::sort(got.begin(), got.end());
    CHECK(std::vector<int>({10, 20, 30, 40, 50}) == got);
}

TEST_CASE("Ordered---preserves-input-order") {
    RunStats stats;

    auto r = count(1, 6).spawn()
           | parallel_map<int>(4, [](int n) { return n * 10; },
                               {.ordered = true});

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::await_completion();
    CHECK(std::vector<int>({10, 20, 30, 40, 50}) == got);
}

TEST_CASE("Ordered---type-changing-transform") {
    RunStats stats;

    auto r = count(1, 4).spawn()
           | parallel_map<int, std::string>(
                 2,
                 [](int n) { return std::to_string(n); },
                 {.ordered = true});

    std::vector<std::string> got;
    stats.spawn([r = std::move(r), &got]{
        for (std::string v; r >> v;) got.push_back(std::move(v));
    });

    csp::await_completion();
    CHECK(std::vector<std::string>({"1", "2", "3"}) == got);
}

TEST_CASE("Ordered---variable-cost-work-stays-in-order") {
    RunStats stats;

    // Reverse sleep: item 1 takes longest, item 5 shortest.
    // With demand-driven dispatch, faster items finish first.
    // Ordered mode must still emit 1,2,3,4,5.
    auto r = count(1, 6).spawn()
           | parallel_map<int>(3,
                 [](int n) {
                     csp::sleep(std::chrono::milliseconds(10 * (6 - n)));
                     return n;
                 },
                 {.ordered = true});

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::await_completion();
    CHECK(std::vector<int>({1, 2, 3, 4, 5}) == got);
}

TEST_CASE("Output-death-stops-workers") {
    RunStats stats;

    auto r = count_forever(1).spawn()
           | parallel_map<int>(4, [](int n) { return n; });

    stats.spawn([r = std::move(r)]{
        int v = r.read();
        // parallel_map with 4 workers: first result could be any
        // of the first few values (workers process in parallel).
        CHECK(v >= 1);
        // Drop reader — output dies, workers should stop.
    });

    csp::await_completion();
}

TEST_CASE("Single-worker") {
    RunStats stats;

    auto r = count(1, 4).spawn()
           | parallel_map<int>(1, [](int n) { return n * 2; },
                               {.ordered = true});

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::await_completion();
    CHECK(std::vector<int>({2, 4, 6}) == got);
}

}
