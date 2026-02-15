#include "testutil.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <csp/microthread.h>
#include <csp/timer.h>

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

    csp::channel<int> ch;
    std::atomic<std::thread::id> writer_tid{};
    std::atomic<std::thread::id> reader_tid{};

    csp::spawn([&] {
        writer_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
        auto w = ++ch;
        for (int i = 0; i < 10; ++i) {
            w << i;
        }
    });

    csp::spawn([&] {
        reader_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
        auto r = --ch;
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
            if (which == 2) {
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

    csp::channel<int> ch;
    std::atomic<int> result{0};

    // Writer sleeps then sends.
    csp::spawn([w = ++ch] {
        csp::sleep(15ms);
        w << 42;
    });

    // Reader uses alt with a generous timeout — should get the value.
    csp::spawn([&, r = --ch] {
        auto timeout = csp::after(200ms);
        int val = 0;
        int which = csp::alt(r >> val, timeout >> csp::poke);
        if (which == 1) {
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
        csp::channel<int> ch;
        csp::spawn([w = ++ch] {
            for (int i = 0; i < MSGS_PER_PAIR; ++i) {
                w << i;
            }
        });
        csp::spawn([r = --ch, &total] {
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
        csp::channel<int> ch;
        csp::spawn([w = ++ch, i] { w << i; });
        csp::spawn([r = --ch, &total] {
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
    csp::channel<int> head;
    auto tail = --head;
    for (int s = 0; s < STAGES; ++s) {
        tail = csp::spawn_producer<int>([r = std::move(tail)](auto&& w) {
            for (int v; r >> v;) {
                w << (v + 1);
            }
        });
    }

    // Feed MSGS zeros into the head.
    csp::spawn([w = ++head] {
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

    csp::channel<int> work_ch;
    csp::channel<int64_t> result_ch;

    // Producer: sends MSGS items.
    csp::spawn([w = ++work_ch] {
        for (int i = 0; i < MSGS; ++i) {
            w << i;
        }
    });

    // Workers: each reads from work_ch, squares the value, sends to result_ch.
    for (int i = 0; i < WORKERS; ++i) {
        csp::spawn([r = -work_ch, w = +result_ch] {
            for (int v; r >> v;) {
                w << (int64_t)v * v;
            }
        });
    }
    work_ch.release();

    // Collector: sums all results.
    std::atomic<int64_t> total{0};
    csp::spawn([&total, r = --result_ch] {
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
    csp::channel<int> ch;

    csp::spawn([w = ++ch] {
        for (int i = 0; i < N; ++i) {
            w << 1;
        }
    });

    std::atomic<int> total{0};
    csp::spawn([&total, r = --ch] {
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
            csp_yield();
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

    csp::channel<int> head;
    auto tail = --head;
    for (int i = 0; i < CHAIN_LEN; ++i) {
        tail = csp::spawn_producer<int>([r = std::move(tail)](auto&& w) {
            for (int v; r >> v;) {
                w << (v + 1);
            }
        });
    }

    // I/O must happen in spawned microthreads in M:N mode — the main
    // thread cannot block on channels because its P may have no local work.
    csp::spawn([w = ++head] {
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
        csp::channel<int> a, b;

        // Writer to channel a.
        csp::spawn([w = ++a, i] { w << i; });
        // Writer to channel b.
        csp::spawn([w = ++b, i] { w << i * 10; });

        // Reader uses alt to pick whichever is ready first.
        csp::spawn([&total, ra = --a, rb = --b] {
            int va = 0, vb = 0;
            int which = csp::alt(ra >> va, rb >> vb);
            if (which == 1) total.fetch_add(va, std::memory_order_relaxed);
            else            total.fetch_add(vb, std::memory_order_relaxed);

            // Drain the other channel.
            if (which == 1) {
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

    csp::channel<int> ch;

    for (int p = 0; p < PRODUCERS; ++p) {
        csp::spawn([w = +ch] {
            for (int i = 0; i < MSGS_PER_PRODUCER; ++i) {
                w << 1;
            }
        });
    }

    std::atomic<int> total{0};
    for (int c = 0; c < CONSUMERS; ++c) {
        csp::spawn([r = -ch, &total] {
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
            csp::channel<int> ch;
            csp::spawn([w = ++ch, i] { w << i; });
            csp::spawn([r = --ch, &total] {
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
        csp::channel<int> ch;
        for (int p = 0; p < PRODUCERS; ++p) {
            csp::spawn([w = +ch] {
                for (int i = 0; i < MSGS_PER_PRODUCER; ++i)
                    w << 1;
            });
        }
        std::atomic<int> total{0};
        for (int c = 0; c < CONSUMERS; ++c) {
            csp::spawn([r = -ch, &total] {
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

    std::function<void(int)> go = [&](int depth) {
        count.fetch_add(1, std::memory_order_relaxed);
        if (depth > 0) {
            csp::spawn([&, depth] { go(depth - 1); });
            csp::spawn([&, depth] { go(depth - 1); });
        }
    };
    csp::spawn([&] { go(DEPTH); });

    csp::schedule();

    int expected = (1 << (DEPTH + 1)) - 1;  // 2^(DEPTH+1) - 1
    CHECK_EQ(expected, count.load());

    csp::shutdown_runtime();
}
