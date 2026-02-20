#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("Conflate - passthrough when consumer keeps up") {
    // Consumer reads immediately — every value passes through individually.
    auto [w, r] = chan<int>{};
    auto cr = conflate<int>([](int a, int b) { return a + b; })
                  .spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << 1; w << 2; w << 3;
    });

    CHECK_EQ(1, cr.read());
    CHECK_EQ(2, cr.read());
    CHECK_EQ(3, cr.read());
}

TEST_CASE("Conflate - merges when consumer is slow") {
    RunStats stats;

    auto [w, r] = chan<int>{};
    auto cr = conflate<int>([](int a, int b) { return a + b; })
                  .spawn(std::move(r));

    // Write several values while nobody is reading.
    // The producer will block after the first write (synchronous channel).
    // We need to use imps to get ahead.

    // Write 1, then 2, then 3 rapidly.
    stats.spawn([w = std::move(w)] {
        w << 1; w << 2; w << 3;
    });

    // Let the writer and conflate run a bit so values accumulate.
    while (csp::internal::run()) { }

    // The conflate filter reads 1 into pending, then races write-pending
    // vs read-next. Since no consumer is reading the output, the read-next
    // wins and pending becomes 1+2=3, then 3+3=6.
    CHECK_EQ(6, cr.read());

    cr = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Conflate - flushes on input close") {
    auto [w, r] = chan<int>{};
    auto cr = conflate<int>([](int a, int b) { return a + b; })
                  .spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << 42;
        // Writer closes after single value.
    });

    // The pending value (42) should be flushed on input close.
    CHECK_EQ(42, cr.read());

    int dummy;
    CHECK_FALSE(bool(cr >> dummy));
}

TEST_CASE("Conflate - output death stops filter") {
    RunStats stats;

    auto [w, r] = chan<int>{};
    {
        auto cr = conflate<int>([](int a, int b) { return a + b; })
                      .spawn(std::move(r));
        // Drop cr immediately.
    }

    // First write succeeds (conflate reads into pending), second fails
    // because conflate exits when it sees the output is dead.
    bool second_write_failed = false;
    stats.spawn([&second_write_failed, w = std::move(w)] {
        w << 1;  // consumed by conflate
        if (!(w << 2)) second_write_failed = true;
    });

    while (csp::internal::run()) { }
    CHECK(second_write_failed);
}

TEST_CASE("Conflate - custom merge function") {
    RunStats stats;

    auto [w, r] = chan<std::string>{};
    // Merge by concatenation with separator.
    auto cr = conflate<std::string>([](std::string a, std::string b) {
        return a + "," + b;
    }).spawn(std::move(r));

    stats.spawn([w = std::move(w)] {
        w << std::string("a");
        w << std::string("b");
        w << std::string("c");
    });

    while (csp::internal::run()) { }

    // Values should be merged.
    std::string result = cr.read();
    // Exact result depends on timing, but all values should appear.
    CHECK(result.find('a') != std::string::npos);
    CHECK(result.find('c') != std::string::npos);
}
