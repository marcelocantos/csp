#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("Conflate - passthrough when consumer keeps up") {
    // Consumer reads immediately. With a sum merge function, the sum of
    // received values always equals 1+2+3=6 regardless of merging.
    // We verify no values are lost.
    int total = 0;
    int count = 0;
    csp::run([&] {
        auto [w, r] = chan<int>{};
        auto cr = conflate<int>([](int a, int b) { return a + b; })
                      .spawn(std::move(r));

        csp::spawn([w = std::move(w)] {
            w << 1; w << 2; w << 3;
        });

        // Read until channel closes -- all values received (possibly merged).
        for (int v; cr >> v;) {
            total += v;
            ++count;
        }
    });
    // Sum must always be 6 (1+2+3) regardless of merge count.
    CHECK(6 == total);
    // At least one read (everything could merge into one).
    CHECK(count >= 1);
    // At most 3 reads (no merging at all).
    CHECK(count <= 3);
}

TEST_CASE("Conflate - merges when consumer is slow") {
    RunStats stats;

    int result = 0;

    // Use a barrier to let writer finish before reading the merged result.
    csp::run([&] {
        auto [w, r] = chan<int>{};
        auto cr = conflate<int>([](int a, int b) { return a + b; })
                      .spawn(std::move(r));

        // Write 1, then 2, then 3 rapidly.
        chan<poke_t> barrier;
        stats.spawn([w = std::move(w), bw = barrier.w.copy()] {
            w << 1; w << 2; w << 3;
            // bw destroyed on scope exit -- signals writer done.
        });
        barrier.w = {};

        // Wait for writer to finish (barrier closes when all bw copies die).
        poke_t p;
        barrier.r >> p;

        // The conflate filter reads 1 into pending, then races write-pending
        // vs read-next. Since no consumer was reading the output, the read-next
        // wins and pending becomes 1+2=3, then 3+3=6.
        result = cr.read();
    });

    CHECK(6 == result);
}

TEST_CASE("Conflate - flushes on input close") {
    csp::run([&] {
        auto [w, r] = chan<int>{};
        auto cr = conflate<int>([](int a, int b) { return a + b; })
                      .spawn(std::move(r));

        csp::spawn([w = std::move(w)] {
            w << 42;
            // Writer closes after single value.
        });

        // The pending value (42) should be flushed on input close.
        CHECK(42 == cr.read());

        int dummy;
        CHECK_FALSE(bool(cr >> dummy));
    });
}

TEST_CASE("Conflate - output death stops filter") {
    RunStats stats;

    // Test that after output dies, conflate eventually stops reading input.
    // We write many values; at least one write must fail.
    constexpr int N = 100;
    std::atomic<int> succeeded{0};

    csp::run([&] {
        auto [w, r] = chan<int>{};
        {
            auto cr = conflate<int>([](int a, int b) { return a + b; })
                          .spawn(std::move(r));
            // Drop cr immediately -- output dies.
        }

        // Writer sends N values; once conflate exits, further writes fail.
        stats.spawn([&succeeded, w = std::move(w)] {
            for (int i = 0; i < N; ++i) {
                if (!(w << i)) break;
                succeeded.fetch_add(1, std::memory_order_relaxed);
            }
        });
    });

    // Conflate should have exited before consuming all N values.
    CHECK(succeeded.load() < N);
}

TEST_CASE("Conflate - custom merge function") {
    RunStats stats;

    std::string result;

    // Use a barrier to let writer finish before reading the merged result.
    csp::run([&] {
        auto [w, r] = chan<std::string>{};
        // Merge by concatenation with separator.
        auto cr = conflate<std::string>([](std::string a, std::string b) {
            return a + "," + b;
        }).spawn(std::move(r));

        chan<poke_t> barrier;
        stats.spawn([w = std::move(w), bw = barrier.w.copy()] {
            w << std::string("a");
            w << std::string("b");
            w << std::string("c");
            // bw destroyed on scope exit -- signals writer done.
        });
        barrier.w = {};

        // Wait for writer to finish.
        poke_t p;
        barrier.r >> p;

        // Values should be merged.
        result = cr.read();
    });

    // Exact result depends on timing, but all values should appear.
    CHECK(result.find('a') != std::string::npos);
    CHECK(result.find('c') != std::string::npos);
}
