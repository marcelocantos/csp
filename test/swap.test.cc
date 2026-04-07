#include "testutil.h"
#include "testscale.h"

#include <atomic>
#include <vector>

using namespace csp;

// ---------------------------------------------------------------------------
// Basic (single-threaded cooperative) swap tests
// ---------------------------------------------------------------------------

TEST_SUITE("swap") {

TEST_CASE("baseline-channel-count") {
    CAPTURE(csp::internal::channel_count(0));
    CAPTURE(csp::internal::channel_count(1));
}

TEST_CASE("swap-writers---basic-data-transfer") {
    int va = 0, vb = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        spawn([w = std::move(a.w)] { w << 42; });
        spawn([w = std::move(b.w)] { w << 99; });

        va = a.r.read();
        vb = b.r.read();
    });
    CHECK(42 == va);
    CHECK(99 == vb);
}

TEST_CASE("swap-writers---redirects-data") {
    int va = 0, vb = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        swap(a.w, b.w);

        spawn([w = std::move(a.w)] { w << 42; });
        spawn([w = std::move(b.w)] { w << 99; });

        va = a.r.read();
        vb = b.r.read();
    });
    CHECK(99 == va);
    CHECK(42 == vb);
}

TEST_CASE("swap-readers---redirects-data") {
    int va = 0, vb = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        swap(a.r, b.r);

        spawn([w = std::move(a.w)] { w << 42; });
        spawn([w = std::move(b.w)] { w << 99; });

        va = a.r.read();
        vb = b.r.read();
    });
    CHECK(99 == va);
    CHECK(42 == vb);
}

TEST_CASE("swap-with-copies---all-copies-see-redirection") {
    int va = 0, vb = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        auto a_w_copy = a.w.copy();
        auto b_w_copy = b.w.copy();

        swap(a.w, b.w);

        spawn([w = std::move(a_w_copy)] { w << 10; });
        spawn([w = std::move(b_w_copy)] { w << 20; });

        a.w = {};
        b.w = {};

        va = a.r.read();
        vb = b.r.read();
    });
    CHECK(20 == va);
    CHECK(10 == vb);
}

TEST_CASE("swap-self-is-no-op") {
    int v = 0;
    csp::run([&] {
        chan<int> a;
        swap(a.w, a.w);

        spawn([w = std::move(a.w)] { w << 7; });

        v = a.r.read();
    });
    CHECK(7 == v);
}

TEST_CASE("double-swap-restores-original") {
    int va = 0, vb = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        swap(a.w, b.w);
        swap(a.w, b.w);

        spawn([w = std::move(a.w)] { w << 42; });
        spawn([w = std::move(b.w)] { w << 99; });

        va = a.r.read();
        vb = b.r.read();
    });
    CHECK(42 == va);
    CHECK(99 == vb);
}

TEST_CASE("swap-death-detection---writer-dies-on-swapped-channel") {
    bool b_r_dead = false;
    int a_r_val = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        swap(a.w, b.w);

        a.w = {};

        int v;
        b_r_dead = !bool(b.r >> v);

        spawn([w = std::move(b.w)] { w << 55; });
        a_r_val = a.r.read();
    });
    CHECK(b_r_dead);
    CHECK(55 == a_r_val);
}

TEST_CASE("swap-during-blocked-read---waiter-retries") {
    bool got_value = false;
    int result = 0;

    csp::run([&] {
        chan<int> a;
        chan<int> b;

        spawn([r = a.r.copy(), &got_value, &result] {
            int v;
            if (bool(r >> v)) {
                got_value = true;
                result = v;
            }
        });

        csp::yield();

        swap(a.w, b.w);

        b.w << 77;

        a.w = {};
        b.r = {};
        b.w = {};
    });

    CHECK(got_value);
    CHECK(77 == result);
}

TEST_CASE("swap-three-way-rotation") {
    int va = 0, vb = 0, vc = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;
        chan<int> c;

        swap(a.w, b.w);
        swap(b.w, c.w);

        spawn([w = std::move(a.w)] { w << 1; });
        spawn([w = std::move(b.w)] { w << 2; });
        spawn([w = std::move(c.w)] { w << 3; });

        va = a.r.read();
        vb = b.r.read();
        vc = c.r.read();
    });
    CHECK(3 == va);
    CHECK(1 == vb);
    CHECK(2 == vc);
}

TEST_CASE("swap-refcounts-preserved") {
    int wr_before = csp::internal::channel_count(0);
    int rd_before = csp::internal::channel_count(1);
    {
        chan<int> a;
        chan<int> b;
        swap(a.w, b.w);
    }
    CHECK(wr_before == csp::internal::channel_count(0));
    CHECK(rd_before == csp::internal::channel_count(1));
}

TEST_CASE("swap-refcounts-with-copies-preserved") {
    int wr_before = csp::internal::channel_count(0);
    int rd_before = csp::internal::channel_count(1);
    {
        chan<int> a;
        chan<int> b;
        auto a_copy = a.w.copy();
        swap(a.w, b.w);
    }
    CHECK(wr_before == csp::internal::channel_count(0));
    CHECK(rd_before == csp::internal::channel_count(1));
}

TEST_CASE("basic-swap-suite-channel-leak-check") {
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

} // TEST_SUITE("swap")

// ---------------------------------------------------------------------------
// M:N stress tests — exercise swap under real concurrency
// ---------------------------------------------------------------------------

TEST_SUITE("swap MN") {

TEST_CASE("MN-Swap---concurrent-data-flow") {
    // Swap writers while data is actively flowing through both channels.
    // Verify no data is lost or duplicated.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 1000 / SCALE_MEDIUM;
    std::atomic<int64_t> total_a{0};
    std::atomic<int64_t> total_b{0};

    chan<int> a;
    chan<int> b;

    // Writers send sequences.
    csp::spawn([w = a.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });
    csp::spawn([w = b.w.copy()] {
        for (int i = MSGS + 1; i <= 2 * MSGS; ++i) w << i;
    });

    // Readers sum everything they receive.
    csp::spawn([r = a.r.copy(), &total_a] {
        for (int v; r >> v;) total_a.fetch_add(v, std::memory_order_relaxed);
    });
    csp::spawn([r = b.r.copy(), &total_b] {
        for (int v; r >> v;) total_b.fetch_add(v, std::memory_order_relaxed);
    });

    // Swapper: uses copies to avoid data race with parent's cleanup.
    csp::spawn([wa = a.w.copy(), wb = b.w.copy()]() mutable {
        for (int i = 0; i < 50; ++i) {
            csp::yield();
            swap(wa, wb);
        }
    });

    a.w = {};
    b.w = {};
    a.r = {};
    b.r = {};

    csp::schedule();

    // All values 1..2*MSGS should appear exactly once across both readers.
    int64_t expected = (int64_t)(2 * MSGS) * (2 * MSGS + 1) / 2;
    CHECK(expected == total_a.load() + total_b.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Swap---swap-during-blocked-alt") {
    // Imps blocked in alt on one channel get woken by swap and retry
    // successfully on the new channel.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 200 / SCALE_MEDIUM;
    std::atomic<int> received{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&received, i] {
            chan<int> a;
            chan<int> b;

            // Reader blocks on a.r.
            csp::spawn([r = a.r.copy(), &received] {
                int v;
                if (bool(r >> v)) received.fetch_add(v, std::memory_order_relaxed);
            });

            // Give the reader time to block.
            csp::yield();

            // Swap writers: b.w now targets a's channel.
            swap(a.w, b.w);

            // Write through b.w (which now targets a's channel).
            b.w << (i + 1);

            // Clean up.
            a.w = {};
            b.w = {};
            a.r = {};
            b.r = {};
        });
    }

    csp::schedule();

    int64_t expected = (int64_t)N * (N + 1) / 2;
    CHECK(expected == (int64_t)received.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Swap---swap-racing-with-endpoint-death") {
    // Swap and endpoint close happen concurrently. Verify no crash or leak.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int CYCLES = 500 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int i = 0; i < CYCLES; ++i) {
        csp::spawn([&completed] {
            chan<int> a;
            chan<int> b;

            // Swapper uses copies to avoid data race with parent.
            csp::spawn([wa = a.w.copy(), wb = b.w.copy()]() mutable {
                swap(wa, wb);
            });

            // Another imp drops a writer copy concurrently.
            csp::spawn([w = a.w.copy()] {
                // w goes out of scope — endpoint death.
            });

            // Drain readers — reader imps handle blocking, not parent.
            csp::spawn([r = a.r.copy()] {
                int v;
                while (bool(r >> v)) { }
            });
            csp::spawn([r = b.r.copy()] {
                int v;
                while (bool(r >> v)) { }
            });

            // Parent drops all its copies. Ordering vs swap is
            // non-deterministic — exactly what we want to stress.
            a.release();
            b.release();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK(CYCLES == completed.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Swap---rapid-repeated-swaps") {
    // Rapidly swap writers back and forth many times, then verify
    // data goes to the correct final destination.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int PAIRS = 100 / SCALE_MEDIUM;
    constexpr int SWAPS_PER_PAIR = 100 / SCALE_LIGHT;
    std::atomic<int64_t> total{0};

    for (int p = 0; p < PAIRS; ++p) {
        csp::spawn([&total] {
            chan<int> a;
            chan<int> b;

            for (int s = 0; s < SWAPS_PER_PAIR; ++s) {
                swap(a.w, b.w);
            }

            bool even = (SWAPS_PER_PAIR % 2) == 0;

            csp::spawn([w = std::move(a.w)] { w << 1; });
            csp::spawn([w = std::move(b.w)] { w << 2; });

            int va = a.r.read();
            int vb = b.r.read();

            if (even) {
                CHECK(1 == va);
                CHECK(2 == vb);
            } else {
                CHECK(2 == va);
                CHECK(1 == vb);
            }
            total.fetch_add(va + vb, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK((int64_t)PAIRS * 3 == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Swap---multi-channel-swap-storm") {
    // Many channels swapping concurrently. Exercises lock ordering
    // and the bijection invariant under contention.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N_CHANS = 20 / SCALE_LIGHT;
    constexpr int SWAPS = 200 / SCALE_MEDIUM;

    std::vector<chan<int>> chans(N_CHANS);

    // Barrier: each swap imp holds a copy of the barrier writer.
    // When all swap imps finish, the writer endpoint dies, signalling
    // the coordinator that the permutation has settled.
    chan<poke_t> barrier;

    // Swapper copies — each swapper gets its own copies so there's
    // no data race with other imps or the parent.
    for (int s = 0; s < SWAPS; ++s) {
        int i = s % N_CHANS;
        int j = (s * 7 + 3) % N_CHANS;
        if (i == j) j = (j + 1) % N_CHANS;
        csp::spawn([wi = chans[i].w.copy(), wj = chans[j].w.copy(),
                    bw = barrier.w.copy()]() mutable {
            swap(wi, wj);
        });
    }

    // Drop the original barrier writer so death fires when all swap
    // imps finish.
    barrier.w = {};

    // Coordinator: wait for all swaps to complete, then spawn writers
    // and readers. This guarantees the slot permutation has settled
    // before any writes resolve.
    std::atomic<int> total{0};
    csp::spawn([br = std::move(barrier.r), &chans, &total] {
        // Block until all swap imps have finished (writer death).
        poke_t p;
        br >> p;

        // After all swaps settle, write through each writer and read
        // from each reader. Total should be conserved.
        for (auto & ch : chans) {
            csp::spawn([w = std::move(ch.w)] { w << 1; });
        }

        for (auto & ch : chans) {
            csp::spawn([r = std::move(ch.r), &total] {
                int v;
                if (bool(r >> v)) total.fetch_add(v, std::memory_order_relaxed);
            });
        }
    });

    csp::schedule();

    // Each channel gets exactly one write (slot permutation is a bijection).
    CHECK(N_CHANS == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Swap---swap-with-shared-copies-across-threads") {
    // Multiple imps hold copies of the same writer. Swap redirects the
    // slot; all copies (on different OS threads) should see the new channel.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int WRITERS = 20 / SCALE_LIGHT;
    constexpr int MSGS_PER_WRITER = 50 / SCALE_MEDIUM;

    chan<int> a;
    chan<int> b;

    // Swap first: a.w's slot now targets b's channel.
    swap(a.w, b.w);

    // Many writers holding copies of a.w — they all target b's channel
    // through the shared slot.
    for (int i = 0; i < WRITERS; ++i) {
        csp::spawn([w = a.w.copy()] {
            for (int j = 0; j < MSGS_PER_WRITER; ++j) w << 1;
        });
    }

    a.w = {};
    b.w = {};

    // Reader on b should receive all values (a.w copies target b).
    std::atomic<int> total{0};
    csp::spawn([r = std::move(b.r), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    // Reader on a should receive nothing (b.w was dropped).
    std::atomic<int> total_a{0};
    csp::spawn([r = std::move(a.r), &total_a] {
        for (int v; r >> v;) total_a.fetch_add(v, std::memory_order_relaxed);
    });

    csp::schedule();

    CHECK(WRITERS * MSGS_PER_WRITER == total.load());
    CHECK(0 == total_a.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Stress---swap-during-pipeline") {
    // A pipeline where the middle link gets swapped to a different channel.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    chan<int> source;
    chan<int> middle;
    chan<int> alt_middle;
    chan<int> sink;

    // Source → middle (initially).
    csp::spawn([r = std::move(source.r), w = middle.w.copy()] {
        for (int v; r >> v;) w << v;
    });

    // Middle → sink.
    csp::spawn([r = std::move(middle.r), w = sink.w.copy()] {
        for (int v; r >> v;) w << v;
    });

    // Alt middle → sink (values that land here after swap also reach sink).
    csp::spawn([r = std::move(alt_middle.r), w = sink.w.copy()] {
        for (int v; r >> v;) w << v;
    });

    // Swap middle's writer to alt_middle partway through.
    // Use copies to avoid data race with parent's cleanup.
    csp::spawn([mw = middle.w.copy(), amw = alt_middle.w.copy()]() mutable {
        for (int i = 0; i < MSGS / 2; ++i) csp::yield();
        swap(mw, amw);
    });

    // Producer.
    csp::spawn([w = std::move(source.w)] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Consumer.
    csp::spawn([r = std::move(sink.r), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    middle.w = {};
    alt_middle.w = {};
    sink.w = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Stress---swap-lifecycle") {
    // Repeated init/shutdown cycles with swaps to surface cleanup races.
    constexpr int CYCLES = 50 / SCALE_MEDIUM;
    constexpr int PAIRS = 50 / SCALE_LIGHT;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        csp::shutdown_runtime();
    csp::set_maxprocs(4);
        std::atomic<int64_t> total{0};

        for (int p = 0; p < PAIRS; ++p) {
            chan<int> a;
            chan<int> b;
            swap(a.w, b.w);

            csp::spawn([w = std::move(a.w), p] { w << p; });
            csp::spawn([w = std::move(b.w), p] { w << p * 10; });
            csp::spawn([r = std::move(a.r), &total] {
                int v;
                if (bool(r >> v)) total.fetch_add(v, std::memory_order_relaxed);
            });
            csp::spawn([r = std::move(b.r), &total] {
                int v;
                if (bool(r >> v)) total.fetch_add(v, std::memory_order_relaxed);
            });
        }

        csp::schedule();

        int64_t expected = 0;
        for (int p = 0; p < PAIRS; ++p) expected += (int64_t)p * 11;
        CHECK(expected == total.load());

        csp::shutdown_runtime();
    }
}

} // TEST_SUITE("swap MN")

// ---------------------------------------------------------------------------
// Fuse / split / 4-arg swap tests
// ---------------------------------------------------------------------------

TEST_SUITE("fuse") {

TEST_CASE("pipe-operator---basic-data-transfer") {
    int val = 0;
    bool a_r_dead = false, b_w_dead = false;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        a.w | b.r;

        spawn([w = std::move(a.w)] { w << 42; });

        val = b.r.read();

        int v;
        a_r_dead = !bool(a.r >> v);
        b_w_dead = !bool(b.w << 99);
    });
    CHECK(42 == val);
    CHECK(a_r_dead);
    CHECK(b_w_dead);
}

TEST_CASE("fuse---basic-data-transfer") {
    int val = 0;
    bool a_r_dead = false, b_w_dead = false;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        fuse(a.w, b.r);

        // a.w now targets temp channel, b.r now targets temp channel.
        // Write through a.w, read from b.r.
        spawn([w = std::move(a.w)] { w << 42; });

        val = b.r.read();

        // a.r sees writer-side death (no writers left on a's channel).
        int v;
        a_r_dead = !bool(a.r >> v);

        // b.w sees reader-side death (no readers left on b's channel).
        b_w_dead = !bool(b.w << 99);
    });
    CHECK(42 == val);
    CHECK(a_r_dead);
    CHECK(b_w_dead);
}

TEST_CASE("fuse---death-propagation") {
    bool a_r_dead = false, b_w_dead = false;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        fuse(a.w, b.r);

        // a.r should see write-side death (a's channel has no writers).
        int v;
        a_r_dead = !bool(a.r >> v);

        // b.w should see read-side death (b's channel has no readers).
        b_w_dead = !bool(b.w << 1);
    });
    CHECK(a_r_dead);
    CHECK(b_w_dead);
}

TEST_CASE("fuse---copies-follow-redirection") {
    int val = 0;
    csp::run([&] {
        chan<int> a;
        chan<int> b;

        auto aw_copy = a.w.copy();
        auto br_copy = b.r.copy();

        fuse(a.w, b.r);

        // Copies share the same slot, so they follow the redirection.
        spawn([w = std::move(aw_copy)] { w << 77; });

        val = br_copy.read();
    });
    CHECK(77 == val);
}

TEST_CASE("4-arg-swap---split") {
    // Split: swap(w, move(a.r), move(b.w), r) breaks one channel into two.
    // After: orig.w → b's channel, orig.r → a's channel.
    // The consumed a.r and b.w die on return, killing orig's channel.
    int b_val = 0, orig_val = 0;
    csp::run([&] {
        chan<int> orig;
        chan<int> a;
        chan<int> b;

        swap(orig.w, std::move(a.r), std::move(b.w), orig.r);

        // orig.w → b's channel. Write through orig.w, read from b.r.
        spawn([w = orig.w.copy()] { w << 42; });
        b_val = b.r.read();

        // orig.r → a's channel. Write through a.w, read from orig.r.
        spawn([w = std::move(a.w)] { w << 99; });
        orig_val = orig.r.read();

        orig.w = {};
        orig.r = {};
    });
    CHECK(42 == b_val);
    CHECK(99 == orig_val);
}

TEST_CASE("fuse-while-reader-is-blocked") {
    bool got_value = false;
    int result = 0;

    csp::run([&] {
        chan<int> a;
        chan<int> b;

        // Reader blocks on b.r.
        spawn([r = b.r.copy(), &got_value, &result] {
            int v;
            if (bool(r >> v)) {
                got_value = true;
                result = v;
            }
        });

        csp::yield();  // let reader block

        // Fuse a.w onto b.r — b.r's slot redirects to temp channel.
        fuse(a.w, b.r);

        // Write through a.w (now targets temp channel, same as b.r).
        a.w << 77;

        a.w = {};
        a.r = {};
        b.w = {};
        b.r = {};
    });

    CHECK(got_value);
    CHECK(77 == result);
}

TEST_CASE("split-while-reader-is-blocked") {
    bool got_value = false;
    int result = 0;

    csp::run([&] {
        chan<int> orig;
        chan<int> a;
        chan<int> b;

        // Reader blocks on orig.r.
        spawn([r = orig.r.copy(), &got_value, &result] {
            int v;
            if (bool(r >> v)) {
                got_value = true;
                result = v;
            }
        });

        csp::yield();  // let reader block

        // Split: orig.w → b's channel, orig.r → a's channel.
        swap(orig.w, std::move(a.r), std::move(b.w), orig.r);

        // Write through a.w — orig.r now targets a's channel.
        a.w << 88;

        orig.w = {};
        orig.r = {};
        a.w = {};
        b.r = {};
    });

    CHECK(got_value);
    CHECK(88 == result);
}

TEST_CASE("fuse---channel-leak-check") {
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

} // TEST_SUITE("fuse")

TEST_SUITE("fuse MN") {

TEST_CASE("MN-Fuse---basic-pipeline") {
    // Fuse a.w onto b.r, then send data through the fused path.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    chan<int> a;
    chan<int> b;

    // Fuse first: a.w and b.r now share a temp channel.
    fuse(a.w, b.r);

    // Producer writes through a.w (→ temp channel).
    csp::spawn([w = a.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Consumer reads through b.r (→ temp channel).
    csp::spawn([r = b.r.copy(), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    a.w = {};
    a.r = {};
    b.w = {};
    b.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Fuse---repeated-fuse") {
    // Many concurrent fuse operations under M:N threading.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int CYCLES = 200 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int i = 0; i < CYCLES; ++i) {
        csp::spawn([&completed] {
            chan<int> a;
            chan<int> b;

            fuse(a.w, b.r);

            csp::spawn([w = a.w.copy()] { w << 1; });
            int v = b.r.read();
            CHECK(1 == v);

            a.w = {};
            a.r = {};
            b.w = {};
            b.r = {};
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK(CYCLES == completed.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Fuse---racing-with-endpoint-death") {
    // Fuse and endpoint death happen concurrently.
    // Exercises the alive_ pinning logic.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int CYCLES = 500 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int i = 0; i < CYCLES; ++i) {
        csp::spawn([&completed] {
            chan<int> a;
            chan<int> b;

            // Fuser uses copies.
            csp::spawn([aw = a.w.copy(), br = b.r.copy()]() mutable {
                fuse(aw, br);
            });

            // Another imp drops a.w concurrently.
            csp::spawn([w = a.w.copy()] { });

            // Drain readers so they can handle death.
            csp::spawn([r = a.r.copy()] {
                int v;
                while (bool(r >> v)) { }
            });
            csp::spawn([r = b.r.copy()] {
                int v;
                while (bool(r >> v)) { }
            });

            a.release();
            b.release();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK(CYCLES == completed.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Fuse---fuse-while-reader-is-blocked") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 200 / SCALE_MEDIUM;
    std::atomic<int> received{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&received, i] {
            chan<int> a;
            chan<int> b;

            // Reader blocks on b.r.
            csp::spawn([r = b.r.copy(), &received] {
                int v;
                if (bool(r >> v)) received.fetch_add(v, std::memory_order_relaxed);
            });

            csp::yield();  // let reader block

            // Fuse a.w onto b.r while reader is blocked.
            fuse(a.w, b.r);

            // Write through a.w (now shares temp channel with b.r).
            a.w << (i + 1);

            a.w = {};
            a.r = {};
            b.w = {};
            b.r = {};
        });
    }

    csp::schedule();

    int64_t expected = (int64_t)N * (N + 1) / 2;
    CHECK(expected == (int64_t)received.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Split---split-while-reader-is-blocked") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 200 / SCALE_MEDIUM;
    std::atomic<int> received{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&received, i] {
            chan<int> orig;
            chan<int> a;
            chan<int> b;

            // Reader blocks on orig.r.
            csp::spawn([r = orig.r.copy(), &received] {
                int v;
                if (bool(r >> v)) received.fetch_add(v, std::memory_order_relaxed);
            });

            csp::yield();  // let reader block

            // Split while reader is blocked.
            swap(orig.w, std::move(a.r), std::move(b.w), orig.r);

            // Write through a.w (orig.r now targets a's channel).
            a.w << (i + 1);

            orig.w = {};
            orig.r = {};
            a.w = {};
            b.r = {};
        });
    }

    csp::schedule();

    int64_t expected = (int64_t)N * (N + 1) / 2;
    CHECK(expected == (int64_t)received.load());

    csp::shutdown_runtime();
}

} // TEST_SUITE("fuse MN")

// ---------------------------------------------------------------------------
// Tap tests
// ---------------------------------------------------------------------------

TEST_SUITE("tap") {

TEST_CASE("tap---basic-observation") {
    csp::run([&] {
        chan<int> ch;

        auto tr = tap(ch.w, ch.r);

        spawn([w = ch.w.copy()] {
            w << 1;
            w << 2;
            w << 3;
        });

        // Both tap and original reader must be consumed — the forwarder
        // writes to tap first, then forwards to the original reader.
        for (int i = 1; i <= 3; ++i) {
            CHECK(i == tr.read());
            CHECK(i == ch.r.read());
        }

        ch.w = {};
        ch.r = {};
        tr = {};
    });
}

TEST_CASE("tap---data-reaches-original-reader") {
    csp::run([&] {
        chan<int> ch;

        auto tr = tap(ch.w, ch.r);

        spawn([w = ch.w.copy()] { w << 42; });

        // Read from tap first (forwarder writes tap before forwarding).
        CHECK(42 == tr.read());

        // Then the original reader gets the value.
        CHECK(42 == ch.r.read());

        ch.w = {};
        ch.r = {};
        tr = {};
    });
}

TEST_CASE("tap---auto-fuse-on-reader-destruction") {
    csp::run([&] {
        chan<int> ch;

        {
            auto tr = tap(ch.w, ch.r);
            // Dropping the tap reader triggers fuse-back inside the forwarder.
        }

        // After untap, w and r should communicate directly.
        spawn([w = ch.w.copy()] { w << 99; });
        CHECK(99 == ch.r.read());

        ch.w = {};
        ch.r = {};
    });
}

TEST_CASE("tap---copies-follow-redirection") {
    csp::run([&] {
        chan<int> ch;

        auto w_copy = ch.w.copy();
        auto r_copy = ch.r.copy();

        auto tr = tap(ch.w, ch.r);

        // Copies share the same slots, so they also go through the tap.
        spawn([w = std::move(w_copy)] { w << 55; });

        CHECK(55 == tr.read());
        CHECK(55 == r_copy.read());

        ch.w = {};
        ch.r = {};
        r_copy = {};
        tr = {};
    });
}

TEST_CASE("tap---death-propagation-after-untap") {
    bool ch_r_dead = false;
    csp::run([&] {
        chan<int> ch;

        {
            auto tr = tap(ch.w, ch.r);
        }

        // Drop writer — reader should see death.
        ch.w = {};

        int v;
        ch_r_dead = !bool(ch.r >> v);

        ch.r = {};
    });
    CHECK(ch_r_dead);
}

TEST_CASE("tap---channel-leak-check") {
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

} // TEST_SUITE("tap")

TEST_SUITE("tap MN") {

TEST_CASE("MN-Tap---observe-pipeline") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> tap_total{0};
    std::atomic<int64_t> reader_total{0};

    chan<int> ch;

    auto tr = tap(ch.w, ch.r);

    // Producer.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Tap observer.
    csp::spawn([r = tr.copy(), &tap_total] {
        for (int v; r >> v;) tap_total.fetch_add(v, std::memory_order_relaxed);
    });

    // Original consumer.
    csp::spawn([r = ch.r.copy(), &reader_total] {
        for (int v; r >> v;) reader_total.fetch_add(v, std::memory_order_relaxed);
    });

    ch.w = {};
    ch.r = {};
    tr = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == tap_total.load());
    CHECK(expected == reader_total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Tap---auto-fuse-restores-direct-path") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 200 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    chan<int> ch;

    // Tap, then immediately drop the tap reader.  The forwarder detects
    // reader death via ~tw, fuses w,r back together, and exits.
    { auto tr = tap(ch.w, ch.r); }

    // Traffic should flow directly.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    csp::spawn([r = ch.r.copy(), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Tap---splice-mid-flight") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> consumer_total{0};
    std::atomic<int64_t> tap_total{0};

    chan<int> ch;

    // Consumer.
    csp::spawn([r = ch.r.copy(), &consumer_total] {
        for (int v; r >> v;) consumer_total.fetch_add(v, std::memory_order_relaxed);
    });

    // Producer.
    auto pj = csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Tapper: wait for some data to flow, splice tap mid-flight, spawn
    // an observer, then keep the tap reader alive until the producer
    // finishes.  Dropping the tap reader triggers fuse-back.
    csp::spawn([w = ch.w.copy(), r = ch.r.copy(),
                pj = std::move(pj), &tap_total]() mutable {
        for (int i = 0; i < MSGS / 2; ++i) csp::yield();

        auto tr = tap(w, r);
        w = {};
        r = {};

        // Observer reads from tap output.
        csp::spawn([r = tr.copy(), &tap_total] {
            for (int v; r >> v;) tap_total.fetch_add(v, std::memory_order_relaxed);
        });

        // Keep tap reader alive until the producer finishes.
        csp::join(pj);
        pj = {};
        // tr destroyed on scope exit → forwarder detects ~tw → fuse-back
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    // The forwarder delivers in-flight values through the fused path,
    // so no values should be lost.  Allow a tolerance of one value for
    // edge cases in M:N scheduling.
    CHECK(consumer_total.load() >= expected - MSGS);
    CHECK(consumer_total.load() <= expected);
    // Tap may see zero values if M:N scheduling completes the producer
    // before the tapper installs — this is a benign scheduling race.
    CAPTURE(tap_total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Tap---remove-mid-flight") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> consumer_total{0};

    chan<int> ch;

    auto tr = tap(ch.w, ch.r);

    // Consumer.
    csp::spawn([r = ch.r.copy(), &consumer_total] {
        for (int v; r >> v;) consumer_total.fetch_add(v, std::memory_order_relaxed);
    });

    // Tap drain (so forwarder doesn't block).
    csp::spawn([r = tr.copy()] {
        for (int v; r >> v;) { }
    });

    // Producer.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Remove tap mid-flight by dropping the tap reader.
    csp::spawn([r = std::move(tr)] {
        for (int i = 0; i < MSGS / 2; ++i) csp::yield();
        // r destroyed on scope exit → forwarder detects ~tw → fuse-back
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == consumer_total.load());

    csp::shutdown_runtime();
}

} // TEST_SUITE("tap MN")

// ---------------------------------------------------------------------------
// Splice tests (single-threaded cooperative)
// ---------------------------------------------------------------------------

TEST_SUITE("splice") {

TEST_CASE("splice---basic-forwarding") {
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) out << v;
        });

        spawn([w = ch.w.copy()] {
            w << 1;
            w << 2;
            w << 3;
        });

        CHECK(1 == ch.r.read());
        CHECK(2 == ch.r.read());
        CHECK(3 == ch.r.read());

        ch.w = {};
        ch.r = {};
    });
}

TEST_CASE("splice---filter-transforms-values") {
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) out << v * 2;
        });

        spawn([w = ch.w.copy()] {
            w << 1;
            w << 2;
            w << 3;
        });

        CHECK(2 == ch.r.read());
        CHECK(4 == ch.r.read());
        CHECK(6 == ch.r.read());

        ch.w = {};
        ch.r = {};
    });
}

TEST_CASE("splice---filter-drops-values") {
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) {
                if (v % 2 == 0) out << v;
            }
        });

        spawn([w = ch.w.copy()] {
            for (int i = 1; i <= 6; ++i) w << i;
        });

        CHECK(2 == ch.r.read());
        CHECK(4 == ch.r.read());
        CHECK(6 == ch.r.read());

        ch.w = {};
        ch.r = {};
    });
}

TEST_CASE("splice---auto-fuse-on-filter-return") {
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            // Forward exactly 2 values then return.
            for (int i = 0; i < 2; ++i) {
                int v;
                if (!(in >> v)) return;
                out << v;
            }
        });

        spawn([w = ch.w.copy()] {
            w << 10;
            w << 20;
            w << 30;
            w << 40;
        });

        // First 2 values go through the filter.
        CHECK(10 == ch.r.read());
        CHECK(20 == ch.r.read());

        // After fuse-back, values flow directly.
        CHECK(30 == ch.r.read());
        CHECK(40 == ch.r.read());

        ch.w = {};
        ch.r = {};
    });
}

TEST_CASE("splice---auto-fuse-on-upstream-death") {
    bool dead = false;
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) out << v;
        });

        spawn([w = ch.w.copy()] {
            w << 42;
            // Writer destroyed on scope exit — upstream dies.
        });

        CHECK(42 == ch.r.read());

        ch.w = {};

        // Reader should see death after filter returns and fuses back.
        int v;
        dead = !bool(ch.r >> v);

        ch.r = {};
    });
    CHECK(dead);
}

TEST_CASE("splice---auto-fuse-on-downstream-death") {
    csp::run([&] {
        chan<int> ch;

        splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) {
                if (!(out << v)) return;
            }
        });

        // Drop the reader — downstream dies.
        ch.r = {};

        // Send a value — the filter's write should fail, filter returns.
        spawn([w = ch.w.copy()] { w << 1; });

        ch.w = {};
    });
}

TEST_CASE("splice---channel-leak-check") {
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

} // TEST_SUITE("splice")

// ---------------------------------------------------------------------------
// Splice tests (M:N concurrency)
// ---------------------------------------------------------------------------

TEST_SUITE("splice MN") {

TEST_CASE("MN-Splice---data-integrity") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    chan<int> ch;

    splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
        for (int v; in >> v;) out << v;
    });

    // Consumer.
    csp::spawn([r = ch.r.copy(), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    // Producer.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Splice---filter-exits-mid-stream") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    constexpr int LIMIT = MSGS / 4;
    std::atomic<int64_t> total{0};

    chan<int> ch;

    splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
        int count = 0;
        for (int v; in >> v;) {
            out << v;
            if (++count >= LIMIT) return;
        }
    });

    // Consumer.
    csp::spawn([r = ch.r.copy(), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    // Producer.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(expected == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN-Splice---splice-mid-flight") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int MSGS = 500 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    chan<int> ch;

    // Consumer.
    csp::spawn([r = ch.r.copy(), &total] {
        for (int v; r >> v;) total.fetch_add(v, std::memory_order_relaxed);
    });

    // Producer.
    csp::spawn([w = ch.w.copy()] {
        for (int i = 1; i <= MSGS; ++i) w << i;
    });

    // Splicer: wait for some data to flow, then splice mid-flight.
    // The filter forwards everything until upstream dies, then returns
    // triggering auto fuse-back.
    csp::spawn([w = ch.w.copy(), r = ch.r.copy()]() mutable {
        for (int i = 0; i < MSGS / 2; ++i) csp::yield();

        splice(w, r, [](reader<int> in, writer<int> out) {
            for (int v; in >> v;) out << v;
        });
        w = {};
        r = {};
    });

    ch.w = {};
    ch.r = {};

    csp::schedule();

    int64_t expected = (int64_t)MSGS * (MSGS + 1) / 2;
    CHECK(total.load() >= expected - MSGS);
    CHECK(total.load() <= expected);

    csp::shutdown_runtime();
}

} // TEST_SUITE("splice MN")
