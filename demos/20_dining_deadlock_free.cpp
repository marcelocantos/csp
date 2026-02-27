// 20_dining_deadlock_free.cpp — Dining philosophers, deadlock-free via CSP
//
// Each fork is a server imp: it sends a poke (fork available), waits for a
// poke back (fork returned), then loops. Philosophers simply read to pick up
// a fork and write to put it back. Odd philosophers pick up right-then-left;
// even philosophers pick up left-then-right. Channel synchronisation makes
// it structurally impossible for two adjacent philosophers to hold the same
// fork simultaneously.
//
// In std C++ the classic solution requires a global lock ordering protocol
// (always acquire lower-numbered mutex first) enforced by convention — easy
// to get wrong. With CSP each fork is a live imp; the channels are the
// protocol, not documentation.

#include "csp.h"

#include <cstdio>

using namespace csp;

static constexpr int N      = 5;
static constexpr int MEALS  = 3;

int main() {
    spawn([]{
        // Each fork channel: server sends poke (available), philosopher returns poke.
        chan<poke_t> fork_ch[N];

        // Fork server imps.
        for (int i = 0; i < N; ++i) {
            spawn([w = fork_ch[i].w.copy(), r = fork_ch[i].r.copy()]{
                for (;;) {
                    if (!(w << poke)) break;        // offer fork
                    poke_t p;
                    if (!(r >> p)) break;           // wait for return
                }
            });
        }

        // Philosopher imps.
        for (int i = 0; i < N; ++i) {
            // Odd philosophers pick up right fork first to break symmetry.
            int left  = i;
            int right = (i + 1) % N;
            if (i % 2 == 1) std::swap(left, right);

            spawn([i,
                   lw = fork_ch[left].w.copy(),  lr = fork_ch[left].r.copy(),
                   rw = fork_ch[right].w.copy(), rr = fork_ch[right].r.copy()]() mutable {
                for (int meal = 0; meal < MEALS; ++meal) {
                    poke_t p;
                    lr >> p;                        // pick up first fork
                    rr >> p;                        // pick up second fork
                    printf("philosopher %d eating (meal %d)\n", i + 1, meal + 1);
                    lw << poke;                     // put down first fork
                    rw << poke;                     // put down second fork
                }
                printf("philosopher %d done\n", i + 1);
            });
        }
    });

    schedule();
}
