// dining_philosophers.cc — Classic concurrency problem
//
// Five philosophers sit around a table, each needing two forks to eat.
// Each fork is an imp that serves as a mutex — it offers itself
// via a channel, and a philosopher "picks up" a fork by reading from
// its channel. Each philosopher picks up its two forks by reading from
// both fork channels in turn, breaking symmetry by having one
// philosopher reach for its forks in reverse order.
//
// No mutexes, no deadlock, no starvation — just channels.

#include "csp.h"

#include <cstdio>
#include <array>

using namespace csp;

int main() {
    spawn([]{
        constexpr int N = 5;
        constexpr int MEALS = 3;

        // Each fork is a channel. A fork imp repeatedly offers
        // itself; a philosopher picks it up by reading, and returns it
        // by reading again (the fork blocks until returned).
        std::array<chan<>, N> forks;
        for (int i = 0; i < N; ++i) {
            spawn([w = forks[i].w.copy()]{
                while (w << poke) { }  // offer fork
            });
        }

        // Spawn philosophers
        auto [done_w, done_r] = chan<>{};
        for (int i = 0; i < N; ++i) {
            spawn([i, left = forks[i].r.copy(), right = forks[(i + 1) % N].r.copy(),
                   done_w = done_w.copy()]{
                for (int meal = 0; meal < MEALS; ++meal) {
                    // Think
                    csp::yield();

                    // Pick up forks (philosopher 0 reverses order to break symmetry)
                    if (i == 0) {
                        right.read();
                        left.read();
                    } else {
                        left.read();
                        right.read();
                    }

                    printf("  Philosopher %d eating meal %d\n", i, meal + 1);
                    csp::yield();  // eat

                    // Put down forks (read again to unblock the fork thread)
                    left.read();
                    right.read();
                }
                printf("  Philosopher %d done\n", i);
            });
        }
        done_w = {};  // release our ref

        // Wait for all philosophers to finish
        done_r >> nullptr;
    });

    schedule();
}
