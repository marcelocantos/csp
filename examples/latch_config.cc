// latch_config.cc — Live configuration reload
//
// A latch holds the most recent value and serves it to any number
// of readers without blocking the writer. Here, a config producer
// periodically updates a latch, while multiple service microthreads
// read the latest config whenever they need it.
//
// This pattern normally requires std::atomic or mutex-protected
// shared state with careful memory ordering. With CSP, the latch
// combinator handles all synchronization.

#include <csp/csp.h>
#include <csp/part/latch.h>
#include <csp/timer.h>

#include <cstdio>
#include <string>
#include <chrono>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

struct Config {
    int max_connections;
    int timeout_ms;
    std::string mode;
};

int main() {
    spawn([]{
        printf("Live configuration reload:\n");

        // Latch: holds the most recent Config and serves it to readers.
        auto [w, r] = latch<Config>.spawn();

        // Config producer: updates config periodically
        spawn([w = std::move(w)]{
            Config configs[] = {
                {10, 1000, "normal"},
                {20, 500,  "fast"},
                {5,  2000, "safe"},
            };
            for (auto & c : configs) {
                w << c;
                csp::sleep(100ms);
            }
        });

        // Service workers that read config when they need it
        for (int id = 1; id <= 3; ++id) {
            spawn([id, config = r.copy()]{
                for (int i = 0; i < 3; ++i) {
                    csp::sleep(80ms * id);  // different rates
                    Config c;
                    if (config >> c) {
                        printf("  Worker %d: max_conn=%d timeout=%dms mode=%s\n",
                               id, c.max_connections, c.timeout_ms, c.mode.c_str());
                    } else {
                        printf("  Worker %d: config channel closed\n", id);
                        return;
                    }
                }
            });
        }

        // Let everything run
        csp::sleep(500ms);
        printf("  Config reload demo complete\n");
    });

    schedule();
}
