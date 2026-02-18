#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Transform each element through a function.
// map<A, B>(f) converts reader<A> to reader<B>. When A == B, the second
// template parameter can be omitted: map<int>([](int n) { return n + 1; }).
template <typename A, typename B = A, typename F>
auto map(F&& f) {
    return make_filter<A, B>([f = std::forward<F>(f)](reader<A> in, writer<B> out) {
        internal::descr("map");

        for (A a; alt(in >> a, ~out) >= 0 && out << f(a);) { }
    });
}

}
