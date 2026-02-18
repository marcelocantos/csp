#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Rate-limit: forward up to n values per interval, drop excess.
// Budget starts at n (first n values pass immediately).
// Resets every interval via tick().
template <typename T>
auto throttle(csp::clock::duration d, size_t n = 1) {
    return make_filter<T>([d, n](reader<T> in, writer<T> out) {
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
