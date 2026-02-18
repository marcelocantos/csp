#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

// Emit consecutive pairs: (a,b), (b,c), (c,d), ...
// Input of fewer than 2 elements produces no output.
template <typename T>
auto pairwise() {
    return make_filter<T, std::pair<T, T>>(
        [](reader<T> in, writer<std::pair<T, T>> out) {
            internal::descr("pairwise");
            T prev;
            if (csp::alt(in >> prev, ~out) != 0) return;
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                auto p = std::make_pair(std::move(prev), t);
                prev = std::move(t);
                if (!(out << std::move(p))) return;
            }
        });
}

}
