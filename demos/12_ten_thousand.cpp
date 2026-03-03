// 12_ten_thousand.cpp — Spawn 10,000 imps
//
// Each imp receives its ID, squares it, and sends the result to a shared output
// channel. Imps use demand-paged stacks (~4 KB each vs ~1 MB for OS threads),
// so 10,000 imps cost ~40 MB of virtual address space, not 10 GB.
//
// Try this with std::thread: the OS will refuse after a few thousand. Imps are
// multiplexed across a small pool of OS threads by the scheduler — spawning
// 10,000 costs only a stack allocation and a queue push.

#include "csp.h"

#include <cstdio>
#include <chrono>
#include <vector>

using namespace csp;
using namespace std::chrono;

int main() {
    spawn([]{
        constexpr int N = 10000;

        auto [out_w, out_r] = chan<int64_t>{};

        auto t0 = steady_clock::now();

        for (int i = 0; i < N; ++i) {
            spawn([i, w = out_w.copy()]{
                int64_t sq = (int64_t)i * i;
                w << sq;
            });
        }
        // Release main's copy so out_r can exhaust after all imps done.
        out_w = {};

        auto t1 = steady_clock::now();

        int64_t sum = 0;
        for (int64_t v : out_r) {
            sum += v;
        }

        auto t2 = steady_clock::now();

        auto spawn_ms = duration_cast<milliseconds>(t1 - t0).count();
        auto collect_ms = duration_cast<milliseconds>(t2 - t1).count();

        printf("Spawned %d imps in %lldms, collected results in %lldms\n",
               N, (long long)spawn_ms, (long long)collect_ms);

        // Verify: sum of i^2 for i in [0,N) = (N-1)*N*(2N-1)/6
        int64_t expected = (int64_t)(N - 1) * N * (2 * N - 1) / 6;
        printf("Sum of squares: %lld (expected %lld) — %s\n",
               (long long)sum, (long long)expected,
               sum == expected ? "CORRECT" : "WRONG");
    });

    schedule();
}
