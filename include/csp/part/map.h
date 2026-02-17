#pragma once

#include <csp/part/part.h>

namespace csp::part {

    template <typename A, typename B = A, typename F>
    auto map(F&& f) {
        return make_filter<A, B>([f = std::forward<F>(f)](reader<A> in, writer<B> out) {
            internal::descr("map");

            for (A a; alt(in >> a, ~out) > 0 && out << f(a);) { }
        });
    }

}
