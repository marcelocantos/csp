// 09_fan_out.cpp — Work distribution with zero coordination code
//
// 4 workers share one input channel and one output channel. No
// atomic<bool> done, no countdown latch, no condition variable.
// Workers pull jobs, push results, and exit when input closes.
// Output channel closes when all workers finish.

#include "csp.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace csp;

int main() {
    spawn([]{
        auto [jobs_w, jobs_r] = chan<int>{};
        auto [res_w, res_r] = chan<int>{};

        // Source: 12 jobs.
        spawn([w = std::move(jobs_w)]{
            for (int i = 1; i <= 12; ++i) w << i;
        });

        // 4 workers: cube each value.
        for (int i = 0; i < 4; ++i)
            spawn([in = jobs_r.copy(), out = res_w.copy()]{
                for (int v; in >> v;) out << v * v * v;
            });
        jobs_r = {};  // release — workers hold the refs
        res_w = {};

        // Collect and sort.
        std::vector<int> results;
        for (int v; res_r >> v;) results.push_back(v);
        std::sort(results.begin(), results.end());

        printf("  cubes:");
        for (int v : results) printf(" %d", v);
        printf("\n");
    });

    schedule();
}
