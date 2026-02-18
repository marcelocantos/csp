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
        T prev;
        bool has_prev = false;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (!has_prev || !eq(prev, t)) {
                has_prev = true;
                prev = t;
                if (!(out << std::move(t))) return;
            }
        }
    });
}

}
