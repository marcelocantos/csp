#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Flatten a stream of containers into a stream of elements.
// reader<Container<T>> → reader<T>
template <typename T, typename C = std::vector<T>>
auto flatten() {
    return make_filter<C, T>([](reader<C> in, writer<T> out) {
        internal::descr("flatten");
        for (C c; csp::alt(in >> c, ~out) == 1;) {
            for (auto& elem : c) {
                if (!(out << std::move(elem))) return;
            }
        }
    });
}

}
