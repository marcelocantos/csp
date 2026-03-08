#include "testutil.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
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
            // Yield to give the work-stealing scheduler a chance to
            // distribute imps across OS threads.
            for (int j = 0; j < 10; ++j) { csp::yield(); }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(N == done.load());
    // With 4 processors, we should see more than 1 OS thread used.
    CHECK(thread_ids.size() > 1);

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
        CHECK(45 == sum);
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

    CHECK(N == count.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - TimerSleep") {
    using namespace std::chrono_literals;

    csp::init_runtime(4);

    std::atomic<int> done{0};
    constexpr int N = 8;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            csp::sleep(200ms);
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(N == done.load());
    // All N sleeps ran concurrently across 4 workers, so wall time
    // should be well under N * 200ms (sequential time = 1600ms).
    // Allow 3x the sleep duration for scheduler/CI overhead.
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < 600ms);

    csp::shutdown_runtime();
}

TEST_CASE("MN - TimerAfterInAlt") {
    using namespace std::chrono_literals;

    csp::init_runtime(2);

    std::atomic<int> timeouts{0};
    constexpr int N = 4;

    for (int i = 0; i < N; ++i) {
        // Each imp waits on an impossible channel with a short timeout.
        csp::spawn([&] {
            csp::writer<int> impossible;
            auto r = --impossible;
            auto timeout = csp::after(10ms);
            int val = 0;
            int which = csp::alt(r >> val, timeout >> nullptr);
            if (which == 1) {
                timeouts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    CHECK(N == timeouts.load());

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

    CHECK(3 == ticks_received.load());

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
        int which = csp::alt(r >> val, timeout >> nullptr);
        if (which == 0) {
            result.store(val, std::memory_order_relaxed);
        }
    });

    csp::schedule();

    CHECK(42 == result.load());

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
    CHECK(expected == total.load());

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
    CHECK(N == count.load());

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
    CHECK(expected == total.load());

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
    CHECK((int64_t)MSGS * STAGES == sum.load());

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
    CHECK(expected == total.load());

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
    CHECK(N == total.load());

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
    CHECK(N == count.load());

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

    // I/O must happen in spawned imps in M:N mode — the main
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

    CHECK((int64_t)CHAIN_LEN * MSGS == total.load());

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
    CHECK(expected == (int64_t)total.load());

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

    CHECK(PRODUCERS * MSGS_PER_PRODUCER == total.load());

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
        CHECK(SPAWNS == count.load());
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
        CHECK(expected == total.load());
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
        CHECK(PRODUCERS * MSGS_PER_PRODUCER == total.load());
        csp::shutdown_runtime();
    }
}

TEST_CASE("MN Volume - SpawnDuringExecution") {
    csp::init_runtime(4);

    // Each imp spawns one child before exiting.
    // Starting from 1, this creates a tree of 2^DEPTH - 1 imps.
    constexpr int DEPTH = (CSP_TEST_SANITIZER ? 12 : 17);  // 131071 or 8191 imps
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
    CHECK(expected == count.load());

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
        CHECK(elapsed < (CSP_TEST_SANITIZER ? 1000ms : 200ms));
        timer_fired.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(timer_fired.load());
    CHECK(stall_done.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// Coverage gap tests — concurrency coverage audit
// ---------------------------------------------------------------------------

TEST_CASE("MN - SingleWorker") {
    csp::init_runtime(1);

    auto [w, r] = csp::chan<int>{};
    std::atomic<int> result{0};

    csp::spawn([w = std::move(w)] {
        for (int i = 0; i < 10; ++i) {
            w << i;
        }
    });

    csp::spawn([&result, r = std::move(r)] {
        int sum = 0;
        for (int v; r >> v;) {
            sum += v;
        }
        result.store(sum, std::memory_order_relaxed);
    });

    csp::schedule();

    CHECK(45 == result.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - ExceptionPropagation") {
    csp::init_runtime(2);

    std::atomic<bool> caught{false};

    csp::spawn([&caught] {
        auto handle = csp::spawn([] {
            throw std::runtime_error("test error from imp");
        });

        try {
            csp::join(handle);
        } catch (std::runtime_error const& e) {
            CHECK(std::string(e.what()) == "test error from imp");
            caught.store(true, std::memory_order_relaxed);
        }
    });

    csp::schedule();

    CHECK(caught.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - DynamicScopingInheritance") {
    csp::init_runtime(2);

    static csp::dynamic<int> depth{0};

    std::atomic<int> parent_val{0};
    std::atomic<int> child_val{0};
    std::atomic<int> child_override_val{0};
    constexpr int N = 20;
    std::atomic<int> done{0};

    csp::spawn([&] {
        csp::local l{depth = 42};
        parent_val.store(*depth, std::memory_order_relaxed);

        // Collect child exception handles so we can join them.
        std::vector<csp::reader<std::exception_ptr>> handles;
        for (int i = 0; i < N; ++i) {
            handles.push_back(csp::spawn([&] {
                // Child should inherit parent's value.
                int inherited = *depth;
                child_val.fetch_add(inherited, std::memory_order_relaxed);

                // Child override should not affect parent.
                csp::local l2{depth = 99};
                child_override_val.fetch_add(*depth, std::memory_order_relaxed);

                done.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        // Wait for all children to complete while local scope is still alive.
        csp::join(handles);
    });

    csp::schedule();

    CHECK(42 == parent_val.load());
    CHECK(N * 42 == child_val.load());
    CHECK(N * 99 == child_override_val.load());
    CHECK(N == done.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - ConcurrentSpawnStress") {
    csp::init_runtime(4);

    constexpr int SPAWNERS = 20 / SCALE_LIGHT;
    constexpr int CHILDREN_PER = 50 / SCALE_LIGHT;
    std::atomic<int> total{0};

    for (int s = 0; s < SPAWNERS; ++s) {
        csp::spawn([&] {
            for (int c = 0; c < CHILDREN_PER; ++c) {
                csp::spawn([&] {
                    total.fetch_add(1, std::memory_order_relaxed);
                });
            }
            total.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(SPAWNERS * (1 + CHILDREN_PER) == total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - HighContentionChannel") {
    csp::init_runtime(4);

    constexpr int NUM_WRITERS = 50 / SCALE_LIGHT;
    constexpr int NUM_READERS = 50 / SCALE_LIGHT;
    constexpr int MSGS_PER_WRITER = 20 / SCALE_LIGHT;

    csp::chan<int> ch;
    std::atomic<int64_t> write_sum{0};
    std::atomic<int64_t> read_sum{0};

    for (int i = 0; i < NUM_WRITERS; ++i) {
        csp::spawn([w = ch.w.copy(), &write_sum, i] {
            for (int j = 0; j < MSGS_PER_WRITER; ++j) {
                int val = i * MSGS_PER_WRITER + j;
                write_sum.fetch_add(val, std::memory_order_relaxed);
                w << val;
            }
        });
    }

    for (int i = 0; i < NUM_READERS; ++i) {
        csp::spawn([r = ch.r.copy(), &read_sum] {
            for (int v; r >> v;) {
                read_sum.fetch_add(v, std::memory_order_relaxed);
            }
        });
    }
    ch.release();

    csp::schedule();

    CHECK(write_sum.load() == read_sum.load());
    // Verify the expected total: sum of i*MSGS_PER_WRITER+j for all writers/msgs.
    int64_t total_msgs = (int64_t)NUM_WRITERS * MSGS_PER_WRITER;
    int64_t expected = total_msgs * (total_msgs - 1) / 2;
    CHECK(expected == read_sum.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - ConcurrentBlocking") {
    csp::init_runtime(2);

    constexpr int N = 20 / SCALE_LIGHT;
    std::atomic<int> total{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&total, i] {
            int result = csp::blocking([i] {
                // Simulate a blocking computation.
                int sum = 0;
                for (int j = 0; j <= i; ++j) sum += j;
                return sum;
            });
            total.fetch_add(result, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    // Each imp i contributes i*(i+1)/2. Total = sum_{i=0}^{N-1} i*(i+1)/2.
    int64_t expected = 0;
    for (int i = 0; i < N; ++i) expected += (int64_t)i * (i + 1) / 2;
    CHECK(expected == (int64_t)total.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - HAMTStress") {
    csp::init_runtime(4);

    static csp::dynamic<int> var1{0};
    static csp::dynamic<int> var2{0};

    constexpr int N = 100 / SCALE_LIGHT;
    std::atomic<int> sum1{0};
    std::atomic<int> sum2{0};

    csp::spawn([&] {
        csp::local l{var1 = 10, var2 = 20};

        std::vector<csp::reader<std::exception_ptr>> handles;
        for (int i = 0; i < N; ++i) {
            handles.push_back(csp::spawn([&, i] {
                // Each child inherits parent values, then overrides one.
                int inherited1 = *var1;
                int inherited2 = *var2;
                sum1.fetch_add(inherited1, std::memory_order_relaxed);
                sum2.fetch_add(inherited2, std::memory_order_relaxed);

                // Override var1 with a child-specific value; var2 should remain.
                csp::local l2{var1 = i};
                int overridden = *var1;
                int still_inherited = *var2;
                CHECK(i == overridden);
                CHECK(20 == still_inherited);
            }));
        }

        // Wait for all children while local scope is alive.
        csp::join(handles);
    });

    csp::schedule();

    CHECK(N * 10 == sum1.load());
    CHECK(N * 20 == sum2.load());

    csp::shutdown_runtime();
}

TEST_CASE("MN - StackPoolExhaustion") {
    csp::init_runtime(4);

    // Spawn more imps than the 256-stack pool cache to exercise mmap fallback.
    constexpr int N = (CSP_TEST_SANITIZER ? 300 : 500);
    auto [w, r] = csp::chan<poke_t>{};
    std::atomic<int> live{0};
    std::atomic<int> done{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&, r_copy = r.copy()] {
            live.fetch_add(1, std::memory_order_relaxed);
            // Block until the coordinator signals, keeping all imps alive.
            poke_t p;
            r_copy >> p;
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Coordinator: wait until all imps are live, then unblock them.
    csp::spawn([&, w = std::move(w)] {
        // Yield to let spawned imps start and block on the channel.
        while (live.load(std::memory_order_relaxed) < N) {
            csp::yield();
        }
        // Send N pokes to unblock all imps.
        for (int i = 0; i < N; ++i) {
            w << poke;
        }
    });
    r = {};  // Release our copy so channel closes after coordinator.

    csp::schedule();

    CHECK(N == done.load());

    csp::shutdown_runtime();
}
