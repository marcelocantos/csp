// fibonacci.cc — Communicating generators
//
// Two imps cooperate to produce Fibonacci numbers.
// A generator sends pairs of consecutive Fibonacci numbers;
// a feedback loop adds them and feeds the next pair back.
// The result is an infinite stream of Fibonacci numbers
// produced by pure message passing — no shared state,
// no mutable variables outside each imp's scope.

#include "csp.h"

#include <cstdio>
#include <cstdint>

using namespace csp;

int main() {
    spawn([]{
        constexpr int N = 40;

        auto [w, r] = chan<int64_t>{};
        spawn([w = std::move(w)]{
            int64_t a = 0, b = 1;
            while (w << a) {
                int64_t next = a + b;
                a = b;
                b = next;
            }
        });

        printf("First %d Fibonacci numbers:\n  ", N);
        int count = 0;
        for (int64_t n : r) {
            printf("%lld ", n);
            if (++count >= N) break;
        }
        printf("\n");
    });

    await_completion();
}
