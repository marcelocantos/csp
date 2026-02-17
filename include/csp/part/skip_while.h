#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Drop elements while pred is true, then forward the rest.
template <typename T, typename Pred>
auto skip_while(Pred&& pred) {
    return make_filter<T>([pred = std::forward<Pred>(pred)](reader<T> in, writer<T> out) {
        internal::descr("skip_while");
        bool skipping = true;
        for (T t; csp::alt(in >> t, ~out) == 1;) {
            if (skipping) {
                if (pred(t)) continue;
                skipping = false;
            }
            if (!(out << std::move(t))) return;
        }
    });
}

}
