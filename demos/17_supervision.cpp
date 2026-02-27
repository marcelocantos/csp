// 17_supervision.cpp — Erlang-style auto-restart with on_exit()
//
// A supervised worker that fails on the first three attempts restarts
// automatically. on_exit(restart_policy{...}) installs the policy for the
// current imp; supervised() wraps the callable in a retry loop that honours
// that policy on each exit.
//
// In std C++ fault-tolerant workers require manual try/catch + retry loop +
// backoff timer + restart counter — all wired by hand. With CSP, on_exit()
// and supervised() make the policy declarative: just describe max_restarts
// and the window, then spawn.

#include "csp.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace csp;

int main() {
    spawn([]{
        auto guard = on_exit(restart_policy{
            .max_restarts = 3,
            .window       = std::chrono::seconds(5),
        });

        static int attempt = 0;

        spawn(supervised([]{
            ++attempt;
            printf("attempt %d\n", attempt);
            if (attempt < 4) {
                throw std::runtime_error("transient failure");
            }
            printf("success on attempt %d\n", attempt);
        }));
    });

    schedule();
}
