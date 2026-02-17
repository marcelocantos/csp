#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Forward elements while pred is true, then close output.
template <typename T, typename Pred>
auto take_while(Pred&& pred) {
    return make_filter<T>([pred = std::forward<Pred>(pred)](reader<T> in, writer<T> out) {
        internal::descr("take_while");
        for (T t; csp::alt(in >> t, ~out) == 1;) {
            if (!pred(t)) return;
            if (!(out << std::move(t))) return;
        }
    });
}

}
