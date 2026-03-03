// 06_timeout.cpp — Timeout any channel operation in one line
//
// In std C++, adding a timeout to a blocking operation requires
// condition_variable::wait_for + predicate + manual timeout tracking +
// spurious wakeup handling. In CSP, you just add after(d) to a prialt
// and you're done — works with reads, writes, or any mix of channels.

#include "csp.h"

#include <chrono>
#include <cstdio>

using namespace csp;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        // --- Slow producer, timeout wins ---
        printf("  Slow producer (200ms), 100ms timeout:\n");
        {
            auto [w, r] = chan<int>{};
            spawn([w = std::move(w)]{ csp::sleep(200ms); w << 42; });

            int v;
            switch (prialt(r >> v, after(100ms) >> nullptr)) {
            case 0:  printf("    got %d\n", v);          break;
            case 1:  printf("    timed out (expected)\n"); break;
            }
        }

        // --- Fast producer, data wins ---
        printf("  Fast producer (10ms), 500ms timeout:\n");
        {
            auto [w, r] = chan<int>{};
            spawn([w = std::move(w)]{ csp::sleep(10ms); w << 99; });

            int v;
            switch (prialt(r >> v, after(500ms) >> nullptr)) {
            case 0:  printf("    got %d (expected)\n", v); break;
            case 1:  printf("    timed out\n");            break;
            }
        }

        // --- Timeout a write (reader too slow) ---
        printf("  Write timeout (reader sleeps 200ms):\n");
        {
            auto [w, r] = chan<int>{};
            spawn([r = std::move(r)]{ csp::sleep(200ms); int v; r >> v; });

            switch (prialt(w << 7, after(50ms) >> nullptr)) {
            case 0:  printf("    sent\n");                 break;
            case 1:  printf("    timed out (expected)\n"); break;
            }
        }
    });

    schedule();
}
