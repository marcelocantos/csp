#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Rate-limit: forward up to n values per interval, drop excess.
// Budget starts at n (first n values pass immediately).
// Resets every interval via tick().
// Optional dead_letter: excess values are written here instead of discarded.
template <typename T>
auto throttle(csp::clock::duration d, size_t n = 1, writer<T> dead_letter = {}) {
    return make_filter<T>([d, n, dead_letter = std::move(dead_letter)](reader<T> in, writer<T> out) mutable {
        internal::descr("throttle");
        auto ticker = csp::tick(d);
        size_t remaining = n;

        for (;;) {
            T t;
            csp::clock::time_point tp;
            switch (csp::alt(in >> t, ticker >> tp, ~out)) {
            case 0:  // Value arrived.
                if (remaining > 0) {
                    --remaining;
                    if (!(out << std::move(t))) return;
                } else if (dead_letter) {
                    dead_letter << std::move(t);
                }
                break;
            case 1:  // Tick — reset budget.
                remaining = n;
                break;
            default:  // Input, ticker, or output died.
                return;
            }
        }
    });
}

}
