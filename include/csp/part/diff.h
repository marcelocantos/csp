#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Emit the difference between consecutive values: b-a, c-b, d-c, ...
// Requires operator- on T.  Input of fewer than 2 elements produces no output.
template <typename T>
inline auto const diff = make_filter<T, T>(
    [](reader<T> in, writer<T> out) {
        internal::descr("diff");
        T prev;
        if (csp::alt(in >> prev, ~out) != 0) return;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            auto d = t - prev;
            prev = std::move(t);
            if (!(out << std::move(d))) return;
        }
    });

}
