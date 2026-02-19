#pragma once

#include <csp/part/part.h>

#include <iterator>
#include <type_traits>
#include <vector>

namespace csp::part {

// Stream elements from a container or initializer list.
template <typename C>
auto enumerate(C&& c, bool cyclic = false) {
    using T = std::decay_t<decltype(*std::begin(c))>;
    return make_producer<T>([c = std::forward<C>(c), cyclic](writer<T> sink) {
        internal::descr("enumerate");

        do {
            for (auto const & e : c) {
                if (!(sink << e)) {
                    return;
                }
            }
        } while (cyclic);
    });
}

template <typename T>
auto enumerate(std::initializer_list<T> c, bool cyclic = false) {
    return enumerate(std::vector<T>(c), cyclic);
}

// Stream elements from a container, repeating forever.
template <typename C>
auto cycle(C&& c) {
    return enumerate(std::forward<C>(c), true);
}

template <typename T>
auto cycle(std::initializer_list<T> c) {
    return enumerate(c, true);
}

}
