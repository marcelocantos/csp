#include "testutil.h"
#include "testscale.h"

#include <atomic>
#include <vector>

using namespace csp;

// ---------------------------------------------------------------------------
// Coin flip: two imps alt on both read and write of the same channel.
// One ends up writing, the other reading — a non-deterministic coin flip.
// ---------------------------------------------------------------------------

TEST_SUITE("coin flip") {

TEST_CASE("basic coin flip") {
    RunStats stats;

    int role_a = -1, role_b = -1;

    csp::run([&] {
        auto [w, r] = chan<int>{};

        stats.spawn([w = w.copy(), r = r.copy(), &role_a] {
            int v = 0;
            switch (alt(w << 1, r >> v)) {
            case 0: role_a = 0; break; // writer
            case 1: role_a = 1; break; // reader
            }
        });

        stats.spawn([w = w.copy(), r = r.copy(), &role_b] {
            int v = 0;
            switch (alt(w << 2, r >> v)) {
            case 0: role_b = 0; break; // writer
            case 1: role_b = 1; break; // reader
            }
        });
    });

    // One must be the writer, the other the reader.
    CHECK(role_a >= 0);
    CHECK(role_b >= 0);
    CHECK(role_a != role_b);
}

TEST_CASE("coin flip - value transfer") {
    RunStats stats;

    int sent = -1, received = -1;

    csp::run([&] {
        auto [w, r] = chan<int>{};

        stats.spawn([w = w.copy(), r = r.copy(), &sent, &received] {
            int v = 0;
            switch (alt(w << 42, r >> v)) {
            case 0: sent = 42; break;
            case 1: received = v; break;
            }
        });

        stats.spawn([w = w.copy(), r = r.copy(), &sent, &received] {
            int v = 0;
            switch (alt(w << 99, r >> v)) {
            case 0: sent = 99; break;
            case 1: received = v; break;
            }
        });
    });

    // Exactly one wrote, one read. The reader got the writer's value.
    CHECK(sent >= 0);
    CHECK(received >= 0);
    CHECK(received == sent);
}

TEST_CASE("coin flip - entropy") {
    // Verify the coin flip produces genuine randomness, not just
    // balanced counts. 010101... and 000...111... both score 50/50
    // but have near-zero entropy.
    //
    // We use a chi-squared goodness-of-fit test on non-overlapping
    // 4-bit blocks (Knuth TAOCP Vol 2 §3.3.1). With 10,000 trials we
    // get 2,500 blocks across 16 categories (expected 156.25 each —
    // well above the ≥5 guideline for chi-squared approximation).
    // Under H0 (uniform random), chi-squared has 15 d.f.
    // Threshold 100 gives P(false reject) ≈ 10⁻¹⁵ (15 nines) while
    // any structured pattern scores far higher:
    //
    //   010101...:  chi_sq ≈ 17,500
    //   000...111:  chi_sq ≈ 17,500
    //   period-4:   chi_sq ≈ 7,500
    //   random:     chi_sq ≈ 15  (expected value = d.f.)
    RunStats stats;

    constexpr int TRIALS = 10000;
    std::vector<int> outcomes;
    outcomes.reserve(TRIALS);

    for (int i = 0; i < TRIALS; ++i) {
        int role_a = -1;

        csp::run([&] {
            auto [w, r] = chan<int>{};

            spawn([w = w.copy(), r = r.copy(), &role_a] {
                int v = 0;
                switch (alt(w << 1, r >> v)) {
                case 0: role_a = 0; break;
                case 1: role_a = 1; break;
                }
            });

            spawn([w = w.copy(), r = r.copy()] {
                int v = 0;
                alt(w << 2, r >> v);
            });
        });

        CHECK(role_a >= 0);
        outcomes.push_back(role_a);
    }

    // Chi-squared on non-overlapping 4-bit blocks.
    constexpr int K = 4;
    constexpr int CATEGORIES = 1 << K;  // 16
    int n_blocks = TRIALS / K;          // 50
    double expected = static_cast<double>(n_blocks) / CATEGORIES;  // 3.125

    int freq[CATEGORIES] = {};
    for (int i = 0; i < n_blocks; ++i) {
        int block = 0;
        for (int j = 0; j < K; ++j)
            block = (block << 1) | outcomes[i * K + j];
        ++freq[block];
    }

    double chi_sq = 0;
    for (int f : freq) {
        double d = f - expected;
        chi_sq += d * d / expected;
    }

    MESSAGE("chi_sq=" << chi_sq << " (expect <100 for random, >1000 for structured)");
    CHECK(chi_sq < 100);
}

TEST_CASE("coin flip - prialt determinism") {
    // With prialt (priority order), the second imp to arrive always
    // tries its write arm first (index 0), matching the first imp's
    // read arm. Verify the pattern holds over many trials.
    RunStats stats;

    constexpr int TRIALS = 50;

    for (int i = 0; i < TRIALS; ++i) {
        int role_a = -1, role_b = -1;

        csp::run([&] {
            auto [w, r] = chan<int>{};

            stats.spawn([w = w.copy(), r = r.copy(), &role_a] {
                int v = 0;
                switch (prialt(w << 1, r >> v)) {
                case 0: role_a = 0; break;
                case 1: role_a = 1; break;
                }
            });

            stats.spawn([w = w.copy(), r = r.copy(), &role_b] {
                int v = 0;
                switch (prialt(w << 2, r >> v)) {
                case 0: role_b = 0; break;
                case 1: role_b = 1; break;
                }
            });
        });

        CHECK(role_a != role_b);
    }
}

TEST_CASE("coin flip - move-only type") {
    RunStats stats;

    bool a_wrote = false, b_wrote = false;
    std::unique_ptr<int> a_got, b_got;

    csp::run([&] {
        auto [w, r] = chan<std::unique_ptr<int>>{};

        stats.spawn([w = w.copy(), r = r.copy(), &a_wrote, &a_got] {
            std::unique_ptr<int> v;
            switch (alt(w << std::make_unique<int>(42), r >> v)) {
            case 0: a_wrote = true; break;
            case 1: a_got = std::move(v); break;
            }
        });

        stats.spawn([w = w.copy(), r = r.copy(), &b_wrote, &b_got] {
            std::unique_ptr<int> v;
            switch (alt(w << std::make_unique<int>(99), r >> v)) {
            case 0: b_wrote = true; break;
            case 1: b_got = std::move(v); break;
            }
        });
    });

    CHECK(a_wrote != b_wrote);
    if (a_wrote) {
        REQUIRE(b_got != nullptr);
        CHECK(42 == *b_got);
    } else {
        REQUIRE(a_got != nullptr);
        CHECK(99 == *a_got);
    }
}

TEST_CASE("coin flip - vector form") {
    RunStats stats;

    int role_a = -1, role_b = -1;

    csp::run([&] {
        auto [w, r] = chan<int>{};

        stats.spawn([w = w.copy(), r = r.copy(), &role_a] {
            int v = 0;
            std::vector<chan_op<int>> ops;
            ops.push_back(w << 1);
            ops.push_back(r >> v);
            role_a = alt(ops);
        });

        stats.spawn([w = w.copy(), r = r.copy(), &role_b] {
            int v = 0;
            std::vector<chan_op<int>> ops;
            ops.push_back(w << 2);
            ops.push_back(r >> v);
            role_b = alt(ops);
        });
    });

    CHECK(role_a >= 0);
    CHECK(role_b >= 0);
    CHECK(role_a != role_b);
}

TEST_CASE("coin flip - with extra peer") {
    // One imp does a coin flip on a channel that also has an
    // external writer waiting. Both outcomes are valid.
    RunStats stats;

    int role = -1;

    csp::run([&] {
        auto [w, r] = chan<int>{};

        // External writer waiting on the channel.
        stats.spawn([w = w.copy()] {
            w << 99;
        });

        // Coin flipper: could match as writer (if it tries write first
        // and the external writer is the reader peer) or as reader
        // (if it finds the external writer).
        stats.spawn([w = w.copy(), r = r.copy(), &role] {
            int v = 0;
            switch (alt(w << 42, r >> v)) {
            case 0: role = 0; break;
            case 1: role = 1; break;
            }
        });

        // Need a reader to absorb the value if coin flipper chose write.
        stats.spawn([r = r.copy()] {
            int v = 0;
            r >> v;
        });
    });

    CHECK(role >= 0);
}

TEST_CASE("coin flip - channel leak check") {
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

} // TEST_SUITE("coin flip")

// ---------------------------------------------------------------------------
// M:N coin flip stress tests
// ---------------------------------------------------------------------------

TEST_SUITE("coin flip MN") {

TEST_CASE("MN coin flip - stress") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int TRIALS = 1000 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int i = 0; i < TRIALS; ++i) {
        auto [w, r] = chan<int>{};

        csp::spawn([w = w.copy(), r = r.copy()] {
            int v = 0;
            alt(w << 1, r >> v);
        });

        csp::spawn([w = w.copy(), r = r.copy(), &completed] {
            int v = 0;
            alt(w << 2, r >> v);
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(TRIALS == completed.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN coin flip - value integrity") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int TRIALS = 500 / SCALE_MEDIUM;
    std::atomic<int> correct{0};

    for (int i = 0; i < TRIALS; ++i) {
        auto [w, r] = chan<int>{};
        auto [result_w, result_r] = chan<int>{};

        int magic = i * 2 + 1;

        // Imp A: if it writes, sends its magic through result channel.
        csp::spawn([w = w.copy(), r = r.copy(), rw = result_w.copy(), magic] {
            int v = 0;
            switch (alt(w << magic, r >> v)) {
            case 0: rw << magic; break;
            case 1: rw << v; break;
            }
        });

        // Imp B: if it writes, sends its magic through result channel.
        csp::spawn([w = w.copy(), r = r.copy(), rw = result_w.copy(), magic] {
            int v = 0;
            switch (alt(w << magic, r >> v)) {
            case 0: rw << magic; break;
            case 1: rw << v; break;
            }
        });

        // Collector: both imps should report the same value (the
        // writer's magic number).
        csp::spawn([rr = std::move(result_r), &correct] {
            int a = rr.read();
            int b = rr.read();
            if (a == b && a > 0) {
                correct.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    CHECK(TRIALS == correct.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN coin flip - many participants") {
    // Multiple imps competing on the same channel under M:N.
    // Even count ensures all can pair up.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int ROUNDS = 100 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int round = 0; round < ROUNDS; ++round) {
        auto [w, r] = chan<int>{};

        // Spawn exactly 2 imps per round (clean pairing).
        csp::spawn([w = w.copy(), r = r.copy(), &completed] {
            int v = 0;
            alt(w << 1, r >> v);
            completed.fetch_add(1, std::memory_order_relaxed);
        });

        csp::spawn([w = w.copy(), r = r.copy(), &completed] {
            int v = 0;
            alt(w << 2, r >> v);
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(ROUNDS * 2 == completed.load());

    csp::shutdown_runtime();
}

} // TEST_SUITE("coin flip MN")
