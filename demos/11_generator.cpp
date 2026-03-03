// 11_generator.cpp — Infinite lazy sequences via channel-based generators
//
// An imp that writes to a channel is a generator: it produces values on demand,
// blocking until the consumer is ready. When the consumer stops reading (drops
// the reader), the writer send fails and the generator exits naturally.
//
// Compare with C++20 coroutines: co_yield is single-threaded and requires a
// custom promise/awaitable type. CSP generators compose with operator|,
// multiplex with alt(), and cancel simply by dropping the reader — no
// coroutine_handle bookkeeping, no custom allocators.

#include "csp.h"

#include <cstdio>
#include <cstdint>

using namespace csp;

int main() {
    spawn([]{
        // --- Natural numbers generator ---
        auto [nw, nr] = chan<int>{};
        spawn([w = std::move(nw)]{
            for (int i = 0; w << i; ++i) {}
        });

        printf("First 10 natural numbers: ");
        int count = 0;
        for (int n : nr) {
            printf("%d ", n);
            if (++count >= 10) break;   // dropping nr cancels the generator
        }
        printf("\n");

        // --- Fibonacci generator ---
        auto [fw, fr] = chan<int64_t>{};
        spawn([w = std::move(fw)]{
            int64_t a = 0, b = 1;
            while (w << a) {
                int64_t next = a + b;
                a = b;
                b = next;
            }
        });

        printf("First 15 Fibonacci numbers: ");
        count = 0;
        for (int64_t n : fr) {
            printf("%lld ", (long long)n);
            if (++count >= 15) break;
        }
        printf("\n");
    });

    schedule();
}
