#pragma once

#include <csp/part/part.h>

namespace csp::part {

template <typename T>
struct throttle_config {
    size_t n = 1;
    writer<T> dead_letter = {};
};

// Rate-limit: forward up to n values per trigger, drop (or dead-letter) excess.
// Each value received on the trigger channel resets the remaining budget to n.
// Use with tick(d) for periodic resets: throttle<int>(tick(100ms), {.n = 3}).
template <typename T, typename Trigger = poke_t>
auto throttle(reader<Trigger> trigger, throttle_config<T> cfg = {}) {
    return make_filter<T>([trigger = std::move(trigger),
                           n = cfg.n,
                           dead_letter = std::move(cfg.dead_letter)]
                          (reader<T> in, writer<T> out) mutable {
        internal::descr("throttle");
        size_t remaining = n;

        for (;;) {
            T t;
            Trigger trig;
            switch (csp::alt(in >> t, trigger >> trig, ~out)) {
            case 0:  // Value arrived.
                if (remaining > 0) {
                    --remaining;
                    if (!(out << std::move(t))) return;
                } else if (dead_letter) {
                    dead_letter << std::move(t);
                }
                break;
            case 1:  // Trigger — reset budget.
                remaining = n;
                break;
            default:  // Input, trigger, or output died.
                return;
            }
        }
    });
}

}
