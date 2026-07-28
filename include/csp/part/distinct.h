#pragma once

#include <csp/part/part.h>

#include <functional>

namespace csp::part {

// Suppress consecutive duplicate values. Non-adjacent duplicates pass through.
// Optional equality comparator (default: std::equal_to<T>).
template <typename T, typename Eq = std::equal_to<T>>
auto distinct(Eq eq = {}) {
    return make_filter<T>([eq](reader<T> in, writer<T> out) {
        internal::descr("distinct");
        // Prime with the first element (see part.h: prime, don't flag).
        T prev;
        if (csp::alt(in >> prev, ~out) != 0) return;
        if (!(out << prev)) return;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (!eq(prev, t)) {
                prev = t;
                if (!(out << std::move(t))) return;
            }
        }
    });
}

}
