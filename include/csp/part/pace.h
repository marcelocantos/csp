#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Rate-limited passthrough: ensure at least d between consecutive emissions.
// All values are preserved (backpressure, no drops). The first value passes
// immediately; subsequent values block until the interval has elapsed.
template <typename T>
auto pace(csp::clock::duration d) {
    return make_filter<T>([d](reader<T> in, writer<T> out) {
        internal::descr("pace");
        T t;
        if (csp::alt(in >> t, ~out) != 0) return;
        if (!(out << std::move(t))) return;

        auto next = csp::clock::now() + d;
        while (csp::alt(in >> t, ~out) == 0) {
            csp::sleep_until(next);
            if (!(out << std::move(t))) return;
            next = csp::clock::now() + d;
        }
    });
}

}
