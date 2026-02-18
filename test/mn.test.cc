#include "testutil.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

TEST_CASE("MN - MultipleThreads") {
    csp::init_runtime(4);

    std::mutex mu;
    std::set<std::thread::id> thread_ids;
    constexpr int N = 100;
    std::atomic<int> done{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            auto id = std::this_thread::get_id();
            {
                std::lock_guard<std::mutex> lk(mu);
                thread_ids.insert(id);
            }
            // Do enough work to let the scheduler spread across threads.
            for (volatile int j = 0; j < 1000; ++j) { }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK_EQ(N, done.load());
    // With 4 processors, we should see more than 1 OS thread used.
    CHECK_GT(thread_ids.size(), 1);

    csp::shutdown_runtime();
}

TEST_CASE("MN - CrossThreadChannel") {
    csp::init_runtime(2);

    auto [w, r] = csp::chan<int>{};
    std::atomic<std::thread::id> writer_tid{};
    std::atomic<std::thread::id> reader_tid{};

    csp::spawn([&writer_tid, w = std::move(w)] {
        writer_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
        for (int i = 0; i < 10; ++i) {
            w << i;
        }
    });

    csp::spawn([&reader_tid, r = std::move(r)] {
        reader_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
        int sum = 0;
        for (int v; r >> v;) {
            sum += v;
        }
        CHECK_EQ(45, sum);
    });

    csp::schedule();
    csp::shutdown_runtime();
}

TEST_CASE("MN - RapidSpawnExit") {
    csp::init_runtime(4);

    std::atomic<int> count{0};
    constexpr int N = 500;

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK_EQ(N, count.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - TimerSleep") {
    using namespace std::chrono_literals;

    csp::init_runtime(4);

    std::atomic<int> done{0};
    constexpr int N = 8;

    auto start = csp::clock::now();

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            csp::sleep(20ms);
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK_EQ(N, done.load());
    // All N sleeps ran concurrently across workers, so wall time should be
    // much less than N * 20ms.
    auto elapsed = csp::clock::now() - start;
    CHECK_LT(elapsed, N * 20ms);

    csp::shutdown_runtime();
}

TEST_CASE("MN - TimerAfterInAlt") {
    using namespace std::chrono_literals;

    csp::init_runtime(2);

    std::atomic<int> timeouts{0};
    constexpr int N = 4;

    for (int i = 0; i < N; ++i) {
        // Each microthread waits on an impossible channel with a short timeout.
        csp::spawn([&] {
            csp::writer<int> impossible;
            auto r = --impossible;
            auto timeout = csp::after(10ms);
            int val = 0;
            int which = csp::alt(r >> val, timeout >> csp::poke);
            if (which == 1) {
                timeouts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    CHECK_EQ(N, timeouts.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - TimerTick") {
    using namespace std::chrono_literals;

    csp::init_runtime(2);

    std::atomic<int> ticks_received{0};

    csp::spawn([&] {
        auto ticker = csp::tick(10ms);
        for (int i = 0; i < 3; ++i) {
            ticker.read();
            ticks_received.fetch_add(1, std::memory_order_relaxed);
        }
        ticker = {};
    });

    csp::schedule();

    CHECK_EQ(3, ticks_received.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - ConcurrentTimersAndChannels") {
    using namespace std::chrono_literals;

    csp::init_runtime(4);

    auto [w, r] = csp::chan<int>{};
    std::atomic<int> result{0};

    // Writer sleeps then sends.
    csp::spawn([w = std::move(w)] {
        csp::sleep(15ms);
        w << 42;
    });

    // Reader uses alt with a generous timeout — should get the value.
    csp::spawn([&, r = std::move(r)] {
        auto timeout = csp::after(200ms);
        int val = 0;
        int which = csp::alt(r >> val, timeout >> csp::poke);
        if (which == 0) {
            result.store(val, std::memory_order_relaxed);
        }
    });

    csp::schedule();

    CHECK_EQ(42, result.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - StressChannels") {
    csp::init_runtime(4);

    constexpr int NUM_PAIRS = 20;
    constexpr int MSGS_PER_PAIR = 50;
    std::atomic<int> total{0};

    for (int p = 0; p < NUM_PAIRS; ++p) {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([w = std::move(w)] {
            for (int i = 0; i < MSGS_PER_PAIR; ++i) {
                w << i;
            }
        });
        csp::spawn([r = std::move(r), &total] {
            for (int v; r >> v;) {
                total.fetch_add(v, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    int expected = NUM_PAIRS * (MSGS_PER_PAIR * (MSGS_PER_PAIR - 1) / 2);
    CHECK_EQ(expected, total.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// Volume tests
// ---------------------------------------------------------------------------

TEST_CASE("MN Volume - SpawnExit 1M") {
    csp::init_runtime(4);

    std::atomic<int> count{0};
    constexpr int N = 1'000'000 / SCALE_HEAVY;

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK_EQ(N, count.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - ChannelPairs 10K") {
    csp::init_runtime(4);

    constexpr int N = 10'000 / SCALE_MEDIUM;
    std::atomic<int64_t> total{0};

    for (int i = 0; i < N; ++i) {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([w = std::move(w), i] { w << i; });
        csp::spawn([r = std::move(r), &total] {
            int v;
            if (r >> v) total.fetch_add(v, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    int64_t expected = (int64_t)N * (N - 1) / 2;
    CHECK_EQ(expected, total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - ChannelPipeline") {
    csp::init_runtime(4);

    constexpr int STAGES = 100 / SCALE_LIGHT;
    constexpr int MSGS = 1000 / SCALE_MEDIUM;

    // Build a pipeline: source → stage[0] → stage[1] → ... → stage[N-1] → sink
    // Each stage increments the value by 1.
    csp::chan<int> head;
    auto tail = std::move(head.r);
    for (int s = 0; s < STAGES; ++s) {
        tail = csp::spawn_producer<int>([r = std::move(tail)](auto&& w) {
            for (int v; r >> v;) {
                w << (v + 1);
            }
        });
    }

    // Feed MSGS zeros into the head.
    csp::spawn([w = std::move(head.w)] {
        for (int i = 0; i < MSGS; ++i) {
            w << 0;
        }
    });

    // Consume from the tail and verify.
    std::atomic<int64_t> sum{0};
    csp::spawn([&sum, r = std::move(tail)] {
        for (int v; r >> v;) {
            sum.fetch_add(v, std::memory_order_relaxed);
        }
    });
    head.release();

    csp::schedule();

    // Each of MSGS messages passes through STAGES stages, gaining +1 each.
    CHECK_EQ((int64_t)MSGS * STAGES, sum.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - FanOutFanIn") {
    csp::init_runtime(4);

    constexpr int WORKERS = 50 / SCALE_LIGHT;
    constexpr int MSGS = 10'000 / SCALE_MEDIUM;

    csp::chan<int> work_ch;
    csp::chan<int64_t> result_ch;

    // Producer: sends MSGS items.
    csp::spawn([w = std::move(work_ch.w)] {
        for (int i = 0; i < MSGS; ++i) {
            w << i;
        }
    });

    // Workers: each reads from work_ch, squares the value, sends to result_ch.
    for (int i = 0; i < WORKERS; ++i) {
        csp::spawn([r = work_ch.r.copy(), w = result_ch.w.copy()] {
            for (int v; r >> v;) {
                w << (int64_t)v * v;
            }
        });
    }
    work_ch.release();

    // Collector: sums all results.
    std::atomic<int64_t> total{0};
    csp::spawn([&total, r = std::move(result_ch.r)] {
        for (int64_t v; r >> v;) {
            total.fetch_add(v, std::memory_order_relaxed);
        }
    });
    result_ch.release();

    csp::schedule();

    // sum(i^2, i=0..N-1) = N*(N-1)*(2N-1)/6
    int64_t N = MSGS;
    int64_t expected = N * (N - 1) * (2 * N - 1) / 6;
    CHECK_EQ(expected, total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - ManyChannelMessages") {
    csp::init_runtime(4);

    constexpr int N = 1'000'000 / SCALE_HEAVY;
    auto [w, r] = csp::chan<int>{};

    csp::spawn([w = std::move(w)] {
        for (int i = 0; i < N; ++i) {
            w << 1;
        }
    });

    std::atomic<int> total{0};
    csp::spawn([&total, r = std::move(r)] {
        for (int v; r >> v;) {
            total.fetch_add(v, std::memory_order_relaxed);
        }
    });

    csp::schedule();
    CHECK_EQ(N, total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - SpawnWithYield") {
    csp::init_runtime(4);

    constexpr int N = 100'000 / SCALE_HEAVY;
    std::atomic<int> count{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            csp::yield();
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK_EQ(N, count.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - DaisyChain") {
    csp::init_runtime(4);

    constexpr int CHAIN_LEN = 1000 / SCALE_MEDIUM;
    constexpr int MSGS = 100 / SCALE_LIGHT;

    csp::chan<int> head;
    auto tail = std::move(head.r);
    for (int i = 0; i < CHAIN_LEN; ++i) {
        tail = csp::spawn_producer<int>([r = std::move(tail)](auto&& w) {
            for (int v; r >> v;) {
                w << (v + 1);
            }
        });
    }

    // I/O must happen in spawned microthreads in M:N mode — the main
    // thread cannot block on channels because its P may have no local work.
    csp::spawn([w = std::move(head.w)] {
        for (int i = 0; i < MSGS; ++i) {
            w << 0;
        }
    });

    std::atomic<int64_t> total{0};
    csp::spawn([&total, r = std::move(tail)] {
        for (int v; r >> v;) {
            total.fetch_add(v, std::memory_order_relaxed);
        }
    });
    head.release();

    csp::schedule();

    CHECK_EQ((int64_t)CHAIN_LEN * MSGS, total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - AltSelectStress") {
    csp::init_runtime(4);

    constexpr int N = 1000 / SCALE_MEDIUM;
    std::atomic<int> total{0};

    for (int i = 0; i < N; ++i) {
        auto [a_w, a_r] = csp::chan<int>{};
        auto [b_w, b_r] = csp::chan<int>{};

        // Writer to channel a.
        csp::spawn([w = std::move(a_w), i] { w << i; });
        // Writer to channel b.
        csp::spawn([w = std::move(b_w), i] { w << i * 10; });

        // Reader uses alt to pick whichever is ready first.
        csp::spawn([&total, ra = std::move(a_r), rb = std::move(b_r)] {
            int va = 0, vb = 0;
            int which = csp::alt(ra >> va, rb >> vb);
            if (which == 0) total.fetch_add(va, std::memory_order_relaxed);
            else            total.fetch_add(vb, std::memory_order_relaxed);

            // Drain the other channel.
            if (which == 0) {
                if (rb >> vb) total.fetch_add(vb, std::memory_order_relaxed);
            } else {
                if (ra >> va) total.fetch_add(va, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    // Each iteration contributes i + i*10 = i*11.
    int64_t expected = 0;
    for (int i = 0; i < N; ++i) expected += (int64_t)i * 11;
    CHECK_EQ(expected, (int64_t)total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN Volume - ProducerConsumer") {
    csp::init_runtime(4);

    constexpr int PRODUCERS = 20 / SCALE_LIGHT;
    constexpr int CONSUMERS = 20 / SCALE_LIGHT;
    constexpr int MSGS_PER_PRODUCER = 5000 / SCALE_MEDIUM;

    csp::chan<int> ch;

    for (int p = 0; p < PRODUCERS; ++p) {
        csp::spawn([w = ch.w.copy()] {
            for (int i = 0; i < MSGS_PER_PRODUCER; ++i) {
                w << 1;
            }
        });
    }

    std::atomic<int> total{0};
    for (int c = 0; c < CONSUMERS; ++c) {
        csp::spawn([r = ch.r.copy(), &total] {
            for (int v; r >> v;) {
                total.fetch_add(v, std::memory_order_relaxed);
            }
        });
    }
    ch.release();

    csp::schedule();

    CHECK_EQ(PRODUCERS * MSGS_PER_PRODUCER, total.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// Stress tests — repeated init/shutdown cycles to surface races
// ---------------------------------------------------------------------------

TEST_CASE("MN Stress - Lifecycle") {
    // Exercises the shutdown condvar race (bug #8) and global queue
    // re-init (bug #5) across many rapid init/shutdown cycles.
    constexpr int CYCLES = 100 / SCALE_MEDIUM;
    constexpr int SPAWNS = 500 / SCALE_LIGHT;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        csp::init_runtime(4);
        std::atomic<int> count{0};
        for (int i = 0; i < SPAWNS; ++i)
            csp::spawn([&] { count.fetch_add(1, std::memory_order_relaxed); });
        csp::schedule();
        CHECK_EQ(SPAWNS, count.load());
        csp::shutdown_runtime();
    }
}

TEST_CASE("MN Stress - ChannelPairs") {
    // Exercises the suspending_/wake_pending_ TOCTOU drain (bug #7)
    // across many cycles with many short-lived channel rendezvous pairs.
    constexpr int CYCLES = 20 / SCALE_MEDIUM;
    constexpr int PAIRS = 2000 / SCALE_MEDIUM;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        csp::init_runtime(4);
        std::atomic<int64_t> total{0};
        for (int i = 0; i < PAIRS; ++i) {
            auto [w, r] = csp::chan<int>{};
            csp::spawn([w = std::move(w), i] { w << i; });
            csp::spawn([r = std::move(r), &total] {
                int v;
                if (r >> v) total.fetch_add(v, std::memory_order_relaxed);
            });
        }
        csp::schedule();
        int64_t expected = (int64_t)PAIRS * (PAIRS - 1) / 2;
        CHECK_EQ(expected, total.load());
        csp::shutdown_runtime();
    }
}

TEST_CASE("MN Stress - ProducerConsumer") {
    // Exercises multi-writer/multi-reader channel rendezvous with
    // repeated init/shutdown to surface cross-P scheduling races.
    constexpr int CYCLES = 20 / SCALE_MEDIUM;
    constexpr int PRODUCERS = 10 / SCALE_LIGHT;
    constexpr int CONSUMERS = 10 / SCALE_LIGHT;
    constexpr int MSGS_PER_PRODUCER = 1000 / SCALE_MEDIUM;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        csp::init_runtime(4);
        csp::chan<int> ch;
        for (int p = 0; p < PRODUCERS; ++p) {
            csp::spawn([w = ch.w.copy()] {
                for (int i = 0; i < MSGS_PER_PRODUCER; ++i)
                    w << 1;
            });
        }
        std::atomic<int> total{0};
        for (int c = 0; c < CONSUMERS; ++c) {
            csp::spawn([r = ch.r.copy(), &total] {
                for (int v; r >> v;)
                    total.fetch_add(v, std::memory_order_relaxed);
            });
        }
        ch.release();
        csp::schedule();
        CHECK_EQ(PRODUCERS * MSGS_PER_PRODUCER, total.load());
        csp::shutdown_runtime();
    }
}

TEST_CASE("MN Volume - SpawnDuringExecution") {
    csp::init_runtime(4);

    // Each microthread spawns one child before exiting.
    // Starting from 1, this creates a tree of 2^DEPTH - 1 microthreads.
    constexpr int DEPTH = (CSP_TEST_SANITIZER ? 12 : 17);  // 131071 or 8191 microthreads
    std::atomic<int> count{0};

    auto go = [&](auto & self, int depth) -> void {
        count.fetch_add(1, std::memory_order_relaxed);
        if (depth > 0) {
            csp::spawn([&, depth] { self(self, depth - 1); });
            csp::spawn([&, depth] { self(self, depth - 1); });
        }
    };
    csp::spawn([&] { go(go, DEPTH); });

    csp::schedule();

    int expected = (1 << (DEPTH + 1)) - 1;  // 2^(DEPTH+1) - 1
    CHECK_EQ(expected, count.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// Watchdog / dynamic processor pool tests
// ---------------------------------------------------------------------------

TEST_CASE("MN - Watchdog rescues stalled P") {
    using namespace std::chrono_literals;

    csp::init_runtime(2);

    // Stall both Ps with busy-loops. The watchdog should detect the stalls,
    // add new Ps, and work stealing drains the channel writer so it completes.
    auto [w, r] = csp::chan<int>{};
    std::atomic<bool> stall1_done{false};
    std::atomic<bool> stall2_done{false};
    std::atomic<bool> writer_done{false};

    // Two busy-loop MTs to stall the initial 2 Ps.
    csp::spawn([&stall1_done] {
        auto end = std::chrono::steady_clock::now() + 200ms;
        while (std::chrono::steady_clock::now() < end) { }
        stall1_done.store(true, std::memory_order_relaxed);
    });
    csp::spawn([&stall2_done] {
        auto end = std::chrono::steady_clock::now() + 200ms;
        while (std::chrono::steady_clock::now() < end) { }
        stall2_done.store(true, std::memory_order_relaxed);
    });

    // A writer that should complete even though the Ps are stalled,
    // because the watchdog adds a new P that steals this MT.
    csp::spawn([&writer_done, w = std::move(w)] {
        w << 42;
        writer_done.store(true, std::memory_order_relaxed);
    });

    // Reader consumes.
    csp::spawn([r = std::move(r)] {
        int v;
        r >> v;
    });

    csp::schedule();
    CHECK(stall1_done.load());
    CHECK(stall2_done.load());
    CHECK(writer_done.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - Watchdog rescues timers from stalled P") {
    using namespace std::chrono_literals;

    csp::init_runtime(2);

    std::atomic<bool> timer_fired{false};
    std::atomic<bool> stall_done{false};

    // Stall one P with a busy-loop for 300ms.
    csp::spawn([&stall_done] {
        auto end = std::chrono::steady_clock::now() + 300ms;
        while (std::chrono::steady_clock::now() < end) { }
        stall_done.store(true, std::memory_order_relaxed);
    });

    // A timer-based sleep on what might be the same P. The watchdog should
    // fire the timer even though the P is stalled.
    csp::spawn([&timer_fired] {
        auto start = std::chrono::steady_clock::now();
        csp::sleep(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        // Should fire reasonably close to 50ms, not delayed by 300ms stall.
        CHECK_LT(elapsed, 200ms);
        timer_fired.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(timer_fired.load());
    CHECK(stall_done.load());

    csp::shutdown_runtime();
}
