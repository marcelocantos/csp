// prime_sieve.cc — Dynamic filter pipeline
//
// The quintessential CSP demo, straight from Hoare's 1978 paper.
// Generate natural numbers starting at 2. The first number out is
// prime; spawn a new filter microthread that removes its multiples,
// and repeat. The pipeline grows dynamically — one new concurrent
// stage per prime discovered.
//
// With mutexes you'd need thread-safe queues, worker pools, and
// careful shutdown logic. Here it's 30 lines of straight-line code.

#include <csp/microthread.h>

#include <cstdio>

using namespace csp;

// Generate 2, 3, 4, ...
void generate(writer<int> out) {
    for (int i = 2; out << i; ++i) { }
}

// Copy values from in to out, dropping multiples of prime.
void filter(reader<int> in, writer<int> out, int prime) {
    for (int n; in >> n;) {
        if (n % prime != 0) {
            if (!(out << n)) return;
        }
    }
}

int main() {
    constexpr int N = 100;

    spawn([]{
        channel<int> ch;
        spawn([w = ++ch]{ generate(w); });

        for (int i = 0; i < N; ++i) {
            int prime = (-ch).read();
            printf("%d ", prime);

            // Insert a new filter stage into the pipeline.
            channel<int> next;
            spawn([in = --ch, out = ++next, prime]{
                filter(in, out, prime);
            });
            ch = next;
        }
        printf("\n");
    });

    schedule();
}
