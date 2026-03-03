// 05_select.cpp — Multiplex multiple channels with alt()
//
// In std C++ there is no select() for threads. You'd need one thread per
// source plus a shared queue with mutex + condvar. With CSP, one alt()
// call fairly multiplexes any number of channels — including detecting
// when each source dies (~reader vultures).

#include "csp.h"

#include <cstdio>
#include <vector>

using namespace csp;

int main() {
    spawn([]{
        constexpr int N = 3;
        int counts[] = {3, 5, 4};

        // Spawn N sensors with different amounts of data.
        std::vector<reader<int>> sensors(N);
        for (int i = 0; i < N; ++i) {
            auto [w, r] = chan<int>{};
            sensors[i] = std::move(r);
            spawn([w = std::move(w), id = i, n = counts[i]]{
                for (int j = 1; j <= n; ++j) w << (id + 1) * j;
            });
        }

        // Multiplex all sensors with a dynamic alt.
        int alive = N;
        while (alive > 0) {
            std::vector<chan_op<int>> ops;
            int v;
            for (int i = 0; i < N; ++i)
                ops.push_back(sensors[i] ? (sensors[i] >> v) : chan_op<int>{});

            int idx = alt(ops);
            if (idx >= 0) {
                printf("  sensor-%d: %d\n", idx + 1, v);
            } else {
                int dead = ~idx;
                printf("  sensor-%d closed\n", dead + 1);
                sensors[dead] = {};
                --alive;
            }
        }
        printf("  all sensors done\n");
    });

    schedule();
}
