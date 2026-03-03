// 07_heartbeat.cpp — Dead peer detection via channel death
//
// When an imp exits, its channel endpoints close. A prialt with ~reader
// (vulture) fires immediately when the peer dies — no heartbeat timers,
// no keepalive protocol, no zombie detection threads. The runtime tells
// you directly.

#include "csp.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        // Spawn 3 workers that finish at different times.
        std::vector<reader<std::exception_ptr>> handles;
        int delays[] = {50, 20, 80};  // ms

        for (int i = 0; i < 3; ++i) {
            handles.push_back(spawn([i, d = delays[i]]{
                csp::sleep(std::chrono::milliseconds(d));
                printf("  worker %d finished (after %dms)\n", i, d);
            }));
        }

        // Monitor: detect exits via vultures on join handles.
        int alive = 3;
        while (alive > 0) {
            std::vector<chan_op<std::exception_ptr>> ops;
            for (int i = 0; i < 3; ++i)
                ops.push_back(handles[i] ? ~handles[i] : chan_op<std::exception_ptr>{});

            int idx = ~alt(ops);  // alt returns ~k for vulture events
            printf("  monitor: worker %d exited\n", idx);
            handles[idx] = {};
            --alive;
        }
        printf("  all workers done\n");
    });

    schedule();
}
