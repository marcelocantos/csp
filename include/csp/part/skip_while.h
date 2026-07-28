#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Drop elements while pred is true, then forward the rest.
template <typename T, typename Pred>
auto skip_while(Pred&& pred) {
    return make_filter<T>([pred = std::forward<Pred>(pred)](reader<T> in, writer<T> out) {
        internal::descr("skip_while");
        // Prime: drop the skipped prefix, then enter the plain
        // forwarding loop (see part.h: prime, don't flag).
        T first;
        do {
            if (csp::alt(in >> first, ~out) != 0) return;
        } while (pred(first));
        if (!(out << std::move(first))) return;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (!(out << std::move(t))) return;
        }
    });
}

}
