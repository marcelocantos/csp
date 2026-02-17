// timeout_patterns.cc — Timers as channels
//
// In CSP, timers are just channels. This means you can compose them
// with alt/prialt alongside any other channel operation. Three patterns:
//
// 1. Operation timeout: alt between work and a deadline.
// 2. Periodic heartbeat: interleave work with timed events.
// 3. Timeout-guarded pipeline: kill a pipeline after a duration.
//
// Compare to the typical mess of cancellation tokens, timer callbacks,
// and shared boolean flags.

#include <csp/csp.h>
#include <csp/timer.h>
#include <csp/part/killswitch.h>

#include <cstdio>
#include <chrono>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        // --- Pattern 1: Operation timeout ---
        printf("Pattern 1: Operation timeout\n");
        {
            // Simulate a slow service
            auto [w, r] = chan<int>{};
            spawn([w = std::move(w)]{
                csp::sleep(200ms);  // takes 200ms
                w << 42;
            });

            // Wait up to 100ms
            auto deadline = after(100ms);
            switch (prialt(r >> nullptr, deadline >> nullptr)) {
            case 1:
                printf("  Got result (this shouldn't happen)\n");
                break;
            case 2:
                printf("  Timed out after 100ms (expected)\n");
                break;
            }
        }

        // --- Pattern 2: Periodic heartbeat ---
        printf("\nPattern 2: Periodic heartbeat\n");
        {
            auto [w, r] = chan<int>{};
            spawn([w = std::move(w)]{
                for (int i = 1; i <= 5; ++i) {
                    csp::sleep(30ms);
                    if (!(w << i)) return;
                }
            });

            auto heartbeat = tick(50ms);
            int work_count = 0;
            int heartbeats = 0;
            for (;;) {
                int n;
                clock::time_point t;
                switch (alt(r >> n, heartbeat >> t)) {
                case 1:
                    printf("  Work item: %d\n", n);
                    work_count++;
                    break;
                case -1:
                    goto done;
                case 2:
                    printf("  Heartbeat #%d\n", ++heartbeats);
                    break;
                }
            }
            done:
            printf("  Processed %d items, %d heartbeats\n", work_count, heartbeats);
        }

        // --- Pattern 3: Timeout-guarded pipeline ---
        printf("\nPattern 3: Timeout-guarded pipeline\n");
        {
            // Producer sends numbers forever
            auto source = spawn_producer<int>([](writer<int> w) {
                for (int i = 1; w << i; ++i) {
                    csp::sleep(20ms);
                }
            });

            // Kill the pipeline after 100ms.
            // Killswitch watches for the keepalive writer to die,
            // so we spawn a thread that holds it alive then releases.
            auto [kw, kr] = chan<>{};
            spawn([w = std::move(kw)]{ csp::sleep(100ms); });
            auto guarded = killswitch<int>(std::move(kr)).spawn(std::move(source));

            int count = 0;
            for (int n; guarded >> n;) {
                count++;
            }
            printf("  Pipeline produced %d items before timeout\n", count);
        }
    });

    schedule();
}
