#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Take every Nth element (0-indexed: emit indices 0, n, 2n, ...).
// stride(1) is identity; stride(2) emits every other element.
template <typename T>
auto stride(size_t n) {
    return make_filter<T>([n](reader<T> in, writer<T> out) {
        internal::descr("stride");
        size_t i = 0;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (i == 0) {
                if (!(out << std::move(t))) return;
            }
            if (++i >= n) i = 0;
        }
    });
}

}
