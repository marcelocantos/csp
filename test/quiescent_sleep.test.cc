// Regression test for 🎯T27: main_loop must not busy-spin when quiescent
// without a hook registered.
//
// When all worker procs are parked and no quiescence hook is registered, the
// main thread must sleep in park_cv.wait rather than busy-spinning.
//
// Strategy: spawn an imp that blocks forever on a never-written channel
// (deadlock), then run schedule() for ~1 second under a wall-clock timeout.
// The test asserts that the CPU time consumed by the process is well below
// the wall time, confirming the runtime slept rather than spun.

#include "testutil.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace csp;
using namespace std::chrono_literals;

#ifndef _WIN32
// Returns combined user+system CPU time in seconds for the current process.
static double process_cpu_seconds() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec  + ru.ru_utime.tv_usec  * 1e-6
         + (double)ru.ru_stime.tv_sec  + ru.ru_stime.tv_usec  * 1e-6;
}
#endif

TEST_CASE("QuiescentSleep---NoHookNoBusySpin") {
#ifdef _WIN32
    // getrusage is POSIX; skip on Windows.
    MESSAGE("Skipped on Windows");
#else
    // Never-written channel to create a permanently suspended imp.
    writer<int> never_written;
    auto blocked_r = --never_written;

    // Wall time for the test.
    constexpr auto wall_duration = 1s;
    // Maximum fraction of wall time that is acceptable as CPU usage.
    // We allow 15% to give headroom for scheduler overhead, GC, etc.
    constexpr double max_cpu_fraction = 0.15;

    double cpu_before = process_cpu_seconds();
    auto wall_start   = std::chrono::steady_clock::now();

    // Spawn a daemon timer thread that calls set_maxprocs to zero out the
    // runtime after wall_duration. Using a separate OS thread so the CSP
    // runtime itself is not involved — we just want to unblock schedule().
    std::thread killer([&] {
        std::this_thread::sleep_for(wall_duration);
        // Poke the runtime out of park_cv.wait by decreasing imps:
        // Close the never_written writer so the blocked imp gets unblocked
        // (receives channel-closed signal) and can exit.
        never_written = {};
    });

    // schedule() drives the runtime. The only imp is blocked waiting for
    // never_written; once we close the writer it unblocks and schedule()
    // returns.
    spawn([r = std::move(blocked_r)] () mutable {
        int v;
        r >> v;  // returns false when writer closes
    });
    schedule();

    killer.join();

    double cpu_after = process_cpu_seconds();
    auto wall_end    = std::chrono::steady_clock::now();

    double wall_elapsed = std::chrono::duration<double>(wall_end - wall_start).count();
    double cpu_elapsed  = cpu_after - cpu_before;
    double cpu_fraction = cpu_elapsed / wall_elapsed;

    // Print for diagnostic visibility in CI.
    printf("  QuiescentSleep: wall=%.3fs cpu=%.3fs fraction=%.1f%%\n",
           wall_elapsed, cpu_elapsed, cpu_fraction * 100.0);
    fflush(stdout);

    CHECK(cpu_fraction < max_cpu_fraction);
#endif
}
