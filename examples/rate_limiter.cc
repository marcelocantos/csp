// rate_limiter.cc — Token bucket via tick
//
// A rate limiter built from three CSP primitives:
//   - tick() generates tokens at a fixed rate
//   - buffer accumulates tokens up to a burst limit
//   - A gate reads a token before allowing each request through
//
// This pattern typically requires mutex + condvar + time tracking +
// careful edge-case handling. In CSP, it's a natural composition
// of existing primitives.

#include "csp.h"

#include <cstdio>
#include <chrono>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        printf("Rate limiter: 10 tokens/sec, burst of 3\n\n");

        // Token source: one token every 100ms (10/sec)
        auto tokens = tick(100ms);

        // Buffer up to 3 tokens (burst capacity)
        auto bucket = std::move(tokens) | chan<time_point>(3);

        // Simulate 8 bursty requests
        auto [w, r] = chan<int>{};
        spawn([w = std::move(w)]{
            // First burst: 3 requests at once
            for (int i = 1; i <= 3; ++i) {
                if (!(w << i)) return;
            }
            // Then 5 more spaced out
            for (int i = 4; i <= 8; ++i) {
                csp::sleep(150ms);
                if (!(w << i)) return;
            }
        });

        // Process requests, consuming one token per request
        auto start = std::chrono::steady_clock::now();
        for (int req; r >> req;) {
            // Wait for a token
            bucket >> nullptr;

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            printf("  Request %d served at t=%lldms\n", req, elapsed);
        }
    });

    schedule();
}
