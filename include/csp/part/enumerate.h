#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

    template <typename T, typename C>
    auto enumerate(C&& c, bool cyclic = false) {
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
        return enumerate<T>(std::vector<T>(c), cyclic);
    }

    template <typename T, typename C>
    auto cycle(C&& c) {
        return enumerate<T>(std::forward<C>(c), true);
    }

    template <typename T>
    auto cycle(std::initializer_list<T> c) {
        return enumerate<T>(c, true);
    }

}
