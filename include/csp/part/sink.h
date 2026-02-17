#pragma once

#include <csp/part/part.h>

namespace csp::part {

    // Consume all values with a side-effect function.
    template <typename A, typename F>
    auto sink(F&& f) {
        return make_consumer<A>([f = std::forward<F>(f)](reader<A> in) {
            internal::descr("sink");

            static Logger scope("chan/sink/scope");
            BRAC_SCOPE(scope, "sink", "");

            for (auto a : in) {
                f(a);
            }
        });
    }

    // Consume all values, assigning each to a reference.
    template <typename T>
    auto sinkhole(T& t) {
        return sink<T>([p = &t](T a) { *p = a; });
    }

}
