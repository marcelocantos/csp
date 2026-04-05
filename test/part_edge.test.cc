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

TEST_CASE("Empty-input---batch") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = batch<int>(3).spawn(reader<int>::dead());
        std::vector<int> v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---map") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = map<int, int>([](int n) { return n * 2; }).spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---where") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = where<int>([](int) { return true; }).spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---scan") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = scan<int, int>(0, [](int acc, int v) { return acc + v; })
                     .spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---flatten") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = flatten<int>.spawn(reader<std::vector<int>>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---distinct") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = distinct<int>().spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---take_while") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = take_while<int>([](int) { return true; }).spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---skip_while") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = skip_while<int>([](int) { return true; }).spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---nwise") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = nwise<3, int>().spawn(reader<int>::dead());
        std::tuple<int, int, int> v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---pairwise") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = pairwise<int>.spawn(reader<int>::dead());
        std::pair<int, int> v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---stride") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = stride<int>(2).spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---window") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = window<int>(3).spawn(reader<int>::dead());
        std::vector<int> v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

TEST_CASE("Empty-input---reduce-emits-initial-value") {
    RunStats rs;
    int val = -1;
    bool closed = false;
    csp::run([&]{
        auto r = reduce<int>(0, std::plus<>{}).spawn(reader<int>::dead());
        val = r.read();
        int v;
        closed = !bool(r >> v);
    });
    CHECK(0 == val);
    CHECK(closed);
}

TEST_CASE("Empty-input---unique") {
    RunStats rs;
    bool closed = false;
    csp::run([&]{
        auto r = unique<int>().spawn(reader<int>::dead());
        int v;
        closed = !bool(r >> v);
    });
    CHECK(closed);
}

// ---------------------------------------------------------------------------
// Output reader death tests: consumer drops reader mid-stream.
// ---------------------------------------------------------------------------

TEST_CASE("Output-death---map") {
    RunStats rs;
    int v0 = 0, v1 = 0, v2 = 0;

    csp::run([&]{
        auto r = map<int, int>([](int n) { return n * 2; })
                     .spawn(count_forever(1).spawn());

        v0 = r.read();
        v1 = r.read();
        v2 = r.read();
        // Drop the output reader; combinator should terminate cleanly.
        r = {};
    });

    CHECK(2 == v0);
    CHECK(4 == v1);
    CHECK(6 == v2);
}

TEST_CASE("Output-death---where") {
    RunStats rs;
    int v0 = 0, v1 = 0;

    csp::run([&]{
        auto r = where<int>([](int) { return true; })
                     .spawn(count_forever(1).spawn());

        v0 = r.read();
        v1 = r.read();
        // Drop the output reader.
        r = {};
    });

    CHECK(1 == v0);
    CHECK(2 == v1);
}

TEST_CASE("Output-death---scan") {
    RunStats rs;
    int v0 = 0, v1 = 0, v2 = 0;

    csp::run([&]{
        auto r = scan<int, int>(0, [](int acc, int v) { return acc + v; })
                     .spawn(count_forever(1).spawn());

        v0 = r.read();
        v1 = r.read();
        v2 = r.read();
        // Drop the output reader.
        r = {};
    });

    CHECK(1 == v0);
    CHECK(3 == v1);
    CHECK(6 == v2);
}

TEST_CASE("Output-death---batch") {
    RunStats rs;
    std::vector<int> v;

    csp::run([&]{
        auto r = batch<int>(2).spawn(count_forever(1).spawn());

        v = r.read();
        // Drop after 1 batch.
        r = {};
    });

    CHECK(2 == v.size());
    CHECK(1 == v[0]);
    CHECK(2 == v[1]);
}

// ---------------------------------------------------------------------------
// random_bytes test
// ---------------------------------------------------------------------------

TEST_CASE("random_bytes-produces-correct-chunk-size") {
    RunStats rs;
    size_t s0 = 0, s1 = 0;

    csp::run([&]{
        auto r = rand::random_bytes(64).spawn();

        auto chunk = r.read();
        s0 = chunk.size();

        auto chunk2 = r.read();
        s1 = chunk2.size();

        // Drop reader; producer should terminate cleanly.
        r = {};
    });

    CHECK(64 == s0);
    CHECK(64 == s1);
}

// ---------------------------------------------------------------------------
// Move-only type test
// ---------------------------------------------------------------------------

TEST_CASE("map-with-unique_ptr") {
    RunStats rs;
    int v0 = 0, v1 = 0, v2 = 0;
    bool closed = false;

    csp::run([&]{
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

        v0 = *r.read();
        v1 = *r.read();
        v2 = *r.read();

        std::unique_ptr<int> v;
        closed = !bool(r >> v);
    });

    CHECK(10 == v0);
    CHECK(20 == v1);
    CHECK(30 == v2);
    CHECK(closed);
}

} // TEST_SUITE
