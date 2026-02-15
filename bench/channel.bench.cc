#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench/nanobench.h>

#include <csp/microthread.h>

#include <algorithm>
#include <random>

using namespace csp;

// Number of operations per batch. Each benchmark lambda runs a full
// schedule() cycle performing BATCH send/recv (or alt) operations,
// and nanobench divides elapsed time by BATCH to get per-op cost.
static constexpr int BATCH = 50'000;

int main() {
    ankerl::nanobench::Bench bench;
    bench.warmup(3).minEpochIterations(3);

    // --- Baseline: single channel send/receive ---
    bench.batch(BATCH).run("send/recv", [&] {
        channel<int> ch;
        csp::spawn([w = +ch] {
            for (int i = 0; i < BATCH; i++) w << i;
        });
        int sum = 0;
        csp::spawn([r = -ch, &sum] {
            int n;
            for (int i = 0; i < BATCH; i++) { r >> n; sum += n; }
        });
        ch.release();
        csp::schedule();
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // --- prialt with 2 channels ---
    bench.batch(2 * BATCH).run("prialt/2ch", [&] {
        channel<int> c0, c1;
        csp::spawn([w = +c0] { for (int i = 0; i < BATCH; i++) w << i; });
        csp::spawn([w = +c1] { for (int i = 0; i < BATCH; i++) w << i; });
        int sum = 0;
        csp::spawn([&, r0 = -c0, r1 = -c1] {
            int n;
            for (int i = 0; i < 2 * BATCH; i++) {
                prialt(r0 >> n, r1 >> n);
                sum += n;
            }
        });
        c0.release();
        c1.release();
        csp::schedule();
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // --- alt with 2 channels ---
    bench.batch(2 * BATCH).run("alt/2ch", [&] {
        channel<int> c0, c1;
        csp::spawn([w = +c0] { for (int i = 0; i < BATCH; i++) w << i; });
        csp::spawn([w = +c1] { for (int i = 0; i < BATCH; i++) w << i; });
        int sum = 0;
        csp::spawn([&, r0 = -c0, r1 = -c1] {
            int n;
            for (int i = 0; i < 2 * BATCH; i++) {
                alt(r0 >> n, r1 >> n);
                sum += n;
            }
        });
        c0.release();
        c1.release();
        csp::schedule();
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // --- prialt with 8 channels ---
    static constexpr int K = 8;
    bench.batch(K * BATCH).run("prialt/8ch", [&] {
        channel<int> chs[K];
        for (int k = 0; k < K; k++) {
            csp::spawn([w = +chs[k]] {
                for (int i = 0; i < BATCH; i++) w << i;
            });
        }
        int sum = 0;
        csp::spawn([&, r0 = -chs[0], r1 = -chs[1], r2 = -chs[2], r3 = -chs[3],
                        r4 = -chs[4], r5 = -chs[5], r6 = -chs[6], r7 = -chs[7]] {
            int n;
            for (int i = 0; i < K * BATCH; i++) {
                prialt(r0 >> n, r1 >> n, r2 >> n, r3 >> n,
                       r4 >> n, r5 >> n, r6 >> n, r7 >> n);
                sum += n;
            }
        });
        for (auto & ch : chs) ch.release();
        csp::schedule();
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // --- alt with 8 channels ---
    bench.batch(K * BATCH).run("alt/8ch", [&] {
        channel<int> chs[K];
        for (int k = 0; k < K; k++) {
            csp::spawn([w = +chs[k]] {
                for (int i = 0; i < BATCH; i++) w << i;
            });
        }
        int sum = 0;
        csp::spawn([&, r0 = -chs[0], r1 = -chs[1], r2 = -chs[2], r3 = -chs[3],
                        r4 = -chs[4], r5 = -chs[5], r6 = -chs[6], r7 = -chs[7]] {
            int n;
            for (int i = 0; i < K * BATCH; i++) {
                alt(r0 >> n, r1 >> n, r2 >> n, r3 >> n,
                    r4 >> n, r5 >> n, r6 >> n, r7 >> n);
                sum += n;
            }
        });
        for (auto & ch : chs) ch.release();
        csp::schedule();
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // --- Isolated: RNG + shuffle overhead (no channel work) ---
    bench.batch(1).run("rng+shuffle/2", [&] {
        csp_chanop ops[2] = {};
        std::random_device rdev;
        std::mt19937 rng(rdev());
        std::shuffle(ops, ops + 2, rng);
        ankerl::nanobench::doNotOptimizeAway(ops[0]);
    });

    bench.batch(1).run("rng+shuffle/8", [&] {
        csp_chanop ops[8] = {};
        std::random_device rdev;
        std::mt19937 rng(rdev());
        std::shuffle(ops, ops + 8, rng);
        ankerl::nanobench::doNotOptimizeAway(ops[0]);
    });

    return 0;
}
