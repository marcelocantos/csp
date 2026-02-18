#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Filter elements by predicate.
// Only values for which pred(v) returns true are forwarded.
template <typename T, typename Pred>
auto where(Pred&& pred) {
    return make_filter<T>([pred = std::forward<Pred>(pred)](reader<T> in, writer<T> out) {
        internal::descr("where");

        static Logger log("chan/where");
        CSP_LOG(log, "start");

        for (T t; csp::alt(in >> t, ~out) == 0;) {
            CSP_LOG(log, "loop");
            if (pred(t) && !(out << std::move(t))) {
                break;
            }
        }
        CSP_LOG(log, "finish");
    });
}

}
