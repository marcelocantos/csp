// 15_cancellation.cpp — Cancel a tree of work with a deadline
//
// cancellation(200ms) creates a deadline scope. Three worker imps spawned
// inside the scope each loop on prialt(done(), r >> v): done() fires when the
// scope cancels, allowing workers to exit cleanly. The main imp's sleep()
// throws timed_out when the deadline expires.
//
// Compare with std::stop_token: you must thread the token through every
// function and check it manually. CSP cancellation is inherited automatically
// by all descendant imps via dynamic scoping — no plumbing required.

#include "csp.h"

#include <cstdio>

using namespace csp;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        // Shared channel workers read from (never written to — they just
        // wait for either a value or cancellation).
        auto [w, r] = chan<int>{};

        auto run_worker = [&](int id) {
            spawn([id, r = r.copy()]{
                printf("worker %d: started\n", id);
                int v;
                for (;;) {
                    // done() is a vulture — returns ~0 when fired.
                    if (prialt(done(), r >> v) < 0) {
                        printf("  worker %d: cancelled\n", id);
                        return;
                    }
                    printf("  worker %d: got %d\n", id, v);
                }
            });
        };

        try {
            cancel_guard cg = cancellation(200ms);

            run_worker(1);
            run_worker(2);
            run_worker(3);

            sleep(1s);   // will throw timed_out after 200ms
            printf("  main: this line should not be reached\n");
        } catch (timed_out const&) {
            printf("  main: deadline expired, all workers cancelled\n");
        }

        // Drop writer so workers' channel reads can fail if they weren't
        // already cancelled (belt-and-suspenders cleanup).
        w = {};
    });

    schedule();
}
