// fibonacci.cc — Communicating generators
//
// Two microthreads cooperate to produce Fibonacci numbers.
// A generator sends pairs of consecutive Fibonacci numbers;
// a feedback loop adds them and feeds the next pair back.
// The result is an infinite stream of Fibonacci numbers
// produced by pure message passing — no shared state,
// no mutable variables outside each microthread's scope.

#include <csp/microthread.h>

#include <cstdio>
#include <cstdint>

using namespace csp;

int main() {
    spawn([]{
        constexpr int N = 40;

        channel<int64_t> fibs;
        spawn([out = ++fibs]{
            int64_t a = 0, b = 1;
            while (out << a) {
                int64_t next = a + b;
                a = b;
                b = next;
            }
        });

        printf("First %d Fibonacci numbers:\n  ", N);
        int count = 0;
        for (int64_t n : -fibs) {
            printf("%lld ", n);
            if (++count >= N) break;
        }
        printf("\n");
    });

    schedule();
}
