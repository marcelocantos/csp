#include "testutil.h"

#include <csp.h>

#include <functional>
#include <memory>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_SUITE("PartEdgeCases") {

// ---------------------------------------------------------------------------
// Empty input tests: writer closes immediately, output reader sees clean close.
// ---------------------------------------------------------------------------

TEST_CASE("Empty input - batch") {
    RunStats rs;
    auto r = batch<int>(3).spawn(reader<int>::dead());
    std::vector<int> v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - map") {
    RunStats rs;
    auto r = map<int, int>([](int n) { return n * 2; }).spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - where") {
    RunStats rs;
    auto r = where<int>([](int) { return true; }).spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - scan") {
    RunStats rs;
    auto r = scan<int, int>(0, [](int acc, int v) { return acc + v; })
                 .spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - flatten") {
    RunStats rs;
    auto r = flatten<int>.spawn(reader<std::vector<int>>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - distinct") {
    RunStats rs;
    auto r = distinct<int>().spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - take_while") {
    RunStats rs;
    auto r = take_while<int>([](int) { return true; }).spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - skip_while") {
    RunStats rs;
    auto r = skip_while<int>([](int) { return true; }).spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - nwise") {
    RunStats rs;
    auto r = nwise<3, int>().spawn(reader<int>::dead());
    std::tuple<int, int, int> v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - pairwise") {
    RunStats rs;
    auto r = pairwise<int>.spawn(reader<int>::dead());
    std::pair<int, int> v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - stride") {
    RunStats rs;
    auto r = stride<int>(2).spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - window") {
    RunStats rs;
    auto r = window<int>(3).spawn(reader<int>::dead());
    std::vector<int> v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - reduce emits initial value") {
    RunStats rs;
    auto r = reduce<int>(0, std::plus<>{}).spawn(reader<int>::dead());
    CHECK(0 == r.read());
    int v;
    CHECK_FALSE(bool(r >> v));
}

TEST_CASE("Empty input - unique") {
    RunStats rs;
    auto r = unique<int>().spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(r >> v));
}

// ---------------------------------------------------------------------------
// Output reader death tests: consumer drops reader mid-stream.
// ---------------------------------------------------------------------------

TEST_CASE("Output death - map") {
    RunStats rs;
    auto r = map<int, int>([](int n) { return n * 2; })
                 .spawn(count_forever(1).spawn());

    CHECK(2 == r.read());
    CHECK(4 == r.read());
    CHECK(6 == r.read());
    // Drop the output reader; combinator should terminate cleanly.
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Output death - where") {
    RunStats rs;
    auto r = where<int>([](int) { return true; })
                 .spawn(count_forever(1).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    // Drop the output reader.
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Output death - scan") {
    RunStats rs;
    auto r = scan<int, int>(0, [](int acc, int v) { return acc + v; })
                 .spawn(count_forever(1).spawn());

    CHECK(1 == r.read());
    CHECK(3 == r.read());
    CHECK(6 == r.read());
    // Drop the output reader.
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Output death - batch") {
    RunStats rs;
    auto r = batch<int>(2).spawn(count_forever(1).spawn());

    auto v = r.read();
    CHECK(2 == v.size());
    CHECK(1 == v[0]);
    CHECK(2 == v[1]);
    // Drop after 1 batch.
    r = {};
    while (csp::internal::run()) { }
}

// ---------------------------------------------------------------------------
// random_bytes test
// ---------------------------------------------------------------------------

TEST_CASE("random_bytes produces correct chunk size") {
    RunStats rs;
    auto r = rand::random_bytes(64).spawn();

    auto chunk = r.read();
    CHECK(64 == chunk.size());

    auto chunk2 = r.read();
    CHECK(64 == chunk2.size());

    // Drop reader; producer should terminate cleanly.
    r = {};
    while (csp::internal::run()) { }
}

// ---------------------------------------------------------------------------
// Move-only type test
// ---------------------------------------------------------------------------

TEST_CASE("map with unique_ptr") {
    RunStats rs;

    chan<std::unique_ptr<int>> in;

    auto r = map<std::unique_ptr<int>, std::unique_ptr<int>>(
        [](std::unique_ptr<int> const & p) {
            return std::make_unique<int>(*p * 10);
        }).spawn(std::move(in.r));

    rs.spawn([w = std::move(in.w)]{
        for (int i = 1; i <= 3; ++i) {
            w << std::make_unique<int>(i);
        }
    });

    CHECK(10 == *r.read());
    CHECK(20 == *r.read());
    CHECK(30 == *r.read());

    std::unique_ptr<int> v;
    CHECK_FALSE(bool(r >> v));
}

} // TEST_SUITE
