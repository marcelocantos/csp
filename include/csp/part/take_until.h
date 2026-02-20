#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Forward elements until pred is true (inclusive — emits the terminating element).
template <typename T, typename Pred>
auto take_until(Pred&& pred) {
    return make_filter<T>([pred = std::forward<Pred>(pred)](reader<T> in, writer<T> out) {
        internal::descr("take_until");
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            bool done = pred(t);
            if (!(out << std::move(t))) return;
            if (done) return;
        }
    });
}

}
