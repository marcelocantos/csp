// 19_fake_time.cpp — Test timers without real delays using fake_clock
//
// fake_clock lets tests advance time cooperatively. Bind it inside an
// imp with csp::local, then call fc.run() — it auto-advances through
// all pending sleeps. An "hour-long" sleep completes in microseconds
// of wall time.
//
// In std C++ testing sleep/timeout logic requires either mocking the
// system clock (complex, invasive), speeding up wall time (fragile),
// or just waiting (slow CI). With CSP the clock is a dynamic variable:
// swap it out with fake_clock and control time programmatically.

#include "csp.h"

#include <chrono>
#include <cstdio>

using namespace csp;
using namespace std::chrono;

int main() {
    fake_clock fc;

    auto wall_start = steady_clock::now();

    // Outer imp binds the fake clock; children inherit it.
    spawn([&]{
        csp::local l{csp::clock = &fc};

        spawn([]{
            printf("  sleeping for 1 hour...\n");
            csp::sleep(hours(1));
            printf("  woke up after 1 hour (fake)\n");
        });

        spawn([]{
            printf("  sleeping for 30 minutes...\n");
            csp::sleep(minutes(30));
            printf("  woke up after 30 minutes (fake)\n");
        });
    });

    // fc.run() is a full scheduler: it runs all ready imps, then
    // auto-advances to the next pending timer, repeating until done.
    fc.run();

    auto us = duration_cast<microseconds>(steady_clock::now() - wall_start).count();
    printf("  wall time elapsed: %lld us\n", us);
}
