#include "testutil.h"

#include <iterator>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("Collect - back_inserter") {
    RunStats stats;

    std::vector<int> result;
    auto w = collect<int>(std::back_inserter(result)).spawn();
    stats.spawn([w = std::move(w)] {
        w << 10; w << 20; w << 30;
    });

    while (csp::internal::run()) { }

    REQUIRE_EQ(3, result.size());
    CHECK_EQ(10, result[0]);
    CHECK_EQ(20, result[1]);
    CHECK_EQ(30, result[2]);
}

TEST_CASE("Collect - raw pointer") {
    RunStats stats;

    int buf[3] = {};
    auto w = collect<int>(buf).spawn();
    stats.spawn([w = std::move(w)] {
        w << 1; w << 2; w << 3;
    });

    while (csp::internal::run()) { }

    CHECK_EQ(1, buf[0]);
    CHECK_EQ(2, buf[1]);
    CHECK_EQ(3, buf[2]);
}

TEST_CASE("Collect - pipeline") {
    RunStats stats;

    std::vector<int> result;
    // producer | consumer → callable
    auto pipeline = enumerate(std::vector<int>{4, 5, 6})
                  | collect<int>(std::back_inserter(result));
    csp::spawn(std::move(pipeline));

    while (csp::internal::run()) { }

    REQUIRE_EQ(3, result.size());
    CHECK_EQ(4, result[0]);
    CHECK_EQ(5, result[1]);
    CHECK_EQ(6, result[2]);
}
